#include "serializer/log/data_block_manager.hpp"

#include "utils.hpp"
#include <boost/bind.hpp>

#include "arch/arch.hpp"
#include "concurrency/mutex.hpp"
#include "perfmon.hpp"
#include "serializer/log/log_serializer.hpp"

/* TODO: Right now we perform garbage collection via the do_write() interface on the
log_serializer_t. This leads to bugs in a couple of ways:
1. We have to be sure to get the metadata (repli timestamp, delete bit) right. The data block
   manager shouldn't have to know about that stuff.
2. We have to special-case the serializer so that it allows us to submit do_write()s during
   shutdown. If there were an alternative interface, it could ignore or refuse our GC requests
   when it is shutting down.
Later, rewrite this so that we have a special interface through which to order
garbage collection. */

perfmon_counter_t
    pm_serializer_data_extents("serializer_data_extents"),
    pm_serializer_data_extents_allocated("serializer_data_extents_allocated[dexts]"),
    pm_serializer_data_extents_reclaimed("serializer_data_extents_reclaimed[dexts]"),
    pm_serializer_data_extents_gced("serializer_data_extents_gced[dexts]"),
    pm_serializer_data_blocks_written("serializer_data_blocks_written"),
    pm_serializer_old_garbage_blocks("serializer_old_garbage_blocks"),
    pm_serializer_old_total_blocks("serializer_old_total_blocks");

data_block_manager_t::data_block_manager_t(const log_serializer_dynamic_config_t *_dynamic_config, extent_manager_t *em, log_serializer_t *_serializer, const log_serializer_on_disk_static_config_t *_static_config)
    : shutdown_callback(NULL), state(state_unstarted),
      dynamic_config(_dynamic_config), static_config(_static_config), extent_manager(em), serializer(_serializer),
      next_active_extent(0),
      gc_state(extent_manager->extent_size)
{
    rassert(dynamic_config);
    rassert(static_config);
    rassert(extent_manager);
    rassert(serializer);
}

data_block_manager_t::~data_block_manager_t() {
    rassert(state == state_unstarted || state == state_shut_down);
}

void data_block_manager_t::prepare_initial_metablock(metablock_mixin_t *mb) {

    for (int i = 0; i < MAX_ACTIVE_DATA_EXTENTS; i++) {
        mb->active_extents[i] = NULL_OFFSET;
        mb->blocks_in_active_extent[i] = 0;
    }
}

void data_block_manager_t::start_reconstruct() {
    rassert(state == state_unstarted);
    gc_state.set_step(gc_reconstruct);
}

// Marks the block at the given offset as alive, in the appropriate
// gc_entry in the entries table.  (This is used when we start up, when
// everything is presumed to be garbage, until we mark it as
// non-garbage.)
void data_block_manager_t::mark_live(off64_t offset) {
    int extent_id = static_config->extent_index(offset);
    int block_id = static_config->block_index(offset);

    if (entries.get(extent_id) == NULL) {
        rassert(gc_state.step() == gc_reconstruct);  // This is called at startup.

        gc_entry *entry = new gc_entry(this, extent_id * extent_manager->extent_size);
        entry->state = gc_entry::state_reconstructing;
        reconstructed_extents.push_back(entry);
    }

    /* mark the block as alive */
    entries.get(extent_id)->i_array.set(block_id, 1);
    entries.get(extent_id)->update_g_array(block_id);
}

void data_block_manager_t::end_reconstruct() {
    rassert(state == state_unstarted);
    gc_state.set_step(gc_ready);
}

void data_block_manager_t::start_existing(direct_file_t *file, metablock_mixin_t *last_metablock) {
    rassert(state == state_unstarted);
    dbfile = file;
    gc_io_account_nice.reset(new file_account_t(file, GC_IO_PRIORITY_NICE));
    gc_io_account_high.reset(new file_account_t(file, GC_IO_PRIORITY_HIGH));
    
    /* Reconstruct the active data block extents from the metablock. */
    
    for (unsigned int i = 0; i < MAX_ACTIVE_DATA_EXTENTS; i++) {
        off64_t offset = last_metablock->active_extents[i];
        
        if (offset != NULL_OFFSET) {
            /* It is possible to have an active data block extent with no actual data
            blocks in it. In this case we would not have created a gc_entry for the extent
            yet. */
            if (entries.get(offset / extent_manager->extent_size) == NULL) {
                gc_entry *e = new gc_entry(this, offset);
                e->state = gc_entry::state_reconstructing;
                reconstructed_extents.push_back(e);
            }
            
            active_extents[i] = entries.get(offset / extent_manager->extent_size);
            rassert(active_extents[i]);
            
            /* Turn the extent from a reconstructing extent into an active extent */
            rassert(active_extents[i]->state == gc_entry::state_reconstructing);
            active_extents[i]->state = gc_entry::state_active;
            reconstructed_extents.remove(active_extents[i]);
            
            blocks_in_active_extent[i] = last_metablock->blocks_in_active_extent[i];
        } else {
            active_extents[i] = NULL;
        }
    }
    
    /* Convert any extents that we found live blocks in, but that are not active extents,
    into old extents */
    
    while (gc_entry *entry = reconstructed_extents.head()) {
        reconstructed_extents.remove(entry);
        
        rassert(entry->state == gc_entry::state_reconstructing);
        entry->state = gc_entry::state_old;
        
        entry->our_pq_entry = gc_pq.push(entry);
        
        gc_stats.old_total_blocks += static_config->blocks_per_extent();
        gc_stats.old_garbage_blocks += entry->g_array.count();
    }
    
    state = state_ready;
}

struct dbm_read_ahead_fsm_t :
    public iocallback_t
{
public:
    data_block_manager_t *parent;
    iocallback_t *callback;
    off64_t extent;
    void *read_ahead_buf;
    off64_t read_ahead_size;
    off64_t read_ahead_offset;
    off64_t off_in;
    void *buf_out;

    dbm_read_ahead_fsm_t(data_block_manager_t *p, off64_t _off_in, void *_buf_out, file_account_t *io_account, iocallback_t *cb)
        : parent(p), callback(cb), read_ahead_buf(NULL), off_in(_off_in), buf_out(_buf_out)
    {
        extent = floor_aligned(off_in, parent->static_config->extent_size());

        // Read up to MAX_READ_AHEAD_BLOCKS blocks
        read_ahead_size = std::min<int64_t>(parent->static_config->extent_size(), MAX_READ_AHEAD_BLOCKS * int64_t(parent->static_config->block_size().ser_value()));
        // We divide the extent into chunks of size read_ahead_size, then select the one which contains off_in
        read_ahead_offset = extent + (off_in - extent) / read_ahead_size * read_ahead_size;
        read_ahead_buf = malloc_aligned(read_ahead_size, DEVICE_BLOCK_SIZE);
        parent->dbfile->read_async(read_ahead_offset, read_ahead_size, read_ahead_buf, io_account, this);
    }

    void on_io_complete() {
        rassert(off_in >= read_ahead_offset);
        rassert(off_in < read_ahead_offset + read_ahead_size);
        rassert(divides(parent->static_config->block_size().ser_value(), off_in - read_ahead_offset));

        // Walk over the read ahead buffer and copy stuff...
        for (int64_t current_block = 0; current_block * parent->static_config->block_size().ser_value() < read_ahead_size; ++current_block) {

            const char *current_buf = reinterpret_cast<char *>(read_ahead_buf) + (current_block * parent->static_config->block_size().ser_value());

            const off64_t current_offset = read_ahead_offset + (current_block * parent->static_config->block_size().ser_value());

            // Copy either into buf_out or create a new buffer for read ahead
            if (current_offset == off_in) {
                ls_buf_data_t *data = reinterpret_cast<ls_buf_data_t *>(buf_out);
                --data;
                memcpy(data, current_buf, parent->static_config->block_size().ser_value());
            } else {
                const block_id_t block_id = reinterpret_cast<const ls_buf_data_t *>(current_buf)->block_id;

                // Determine whether the block is live.
                bool block_is_live = block_id != 0;
                // Do this by checking the LBA
                const flagged_off64_t flagged_lba_offset = parent->serializer->lba_index->get_block_offset(block_id);
                block_is_live = block_is_live && flagged_lba_offset.has_value() && current_offset == flagged_lba_offset.get_value();

                if (!block_is_live) {
                    continue;
                }

                const repli_timestamp_t recency_timestamp = parent->serializer->lba_index->get_block_recency(block_id);

                ls_buf_data_t *data = reinterpret_cast<ls_buf_data_t *>(parent->serializer->malloc());
                --data;
                memcpy(data, current_buf, parent->static_config->block_size().ser_value());
                ++data;
                boost::intrusive_ptr<ls_block_token_pointee_t> ls_token(new ls_block_token_pointee_t(parent->serializer, current_offset));
                boost::intrusive_ptr<standard_block_token_t> token = to_standard_block_token(block_id, ls_token);
                if (!parent->serializer->offer_buf_to_read_ahead_callbacks(block_id, data, token, recency_timestamp)) {
                    // If there is no interest anymore, delete the buffer again
                    parent->serializer->free(data);
                    continue;
                }
            }
        }

        free(read_ahead_buf);

        callback->on_io_complete();
        delete this;
    }
};

bool data_block_manager_t::should_perform_read_ahead(off64_t offset) {
    unsigned int extent_id = static_config->extent_index(offset);

    gc_entry *entry = entries.get(extent_id);

    // If the extent was written, we don't perform read ahead because it would
    // a) be potentially useless and b) has an elevated risk of conflicting with
    // active writes on the io queue.
    return !entry->was_written && serializer->should_perform_read_ahead();
}

void data_block_manager_t::read(off64_t off_in, void *buf_out, file_account_t *io_account, iocallback_t *cb) {
    rassert(state == state_ready);

    if (should_perform_read_ahead(off_in)) {
        // We still need an fsm for read ahead as additional work has to be done on io complete...
        new dbm_read_ahead_fsm_t(this, off_in, buf_out, io_account, cb);
    } else {
        ls_buf_data_t *data = reinterpret_cast<ls_buf_data_t *>(buf_out);
        data--;
        dbfile->read_async(off_in, static_config->block_size().ser_value(), data, io_account, cb);
    }
}

/*
 Instead of wrapping this into a coroutine, we are still using a callback as we
 want to be able to spawn a lot of writes in parallel. Having to spawn a coroutine
 for each of them would involve a major memory overhead for all the stacks.
 Also this makes stuff easier in other places, as we get the offset as well as a freshly
 set block_sequence_id immediately.
 */
off64_t data_block_manager_t::write(const void *buf_in, block_id_t block_id, bool assign_new_block_sequence_id,
                                    file_account_t *io_account, iocallback_t *cb) {
    // Either we're ready to write, or we're shutting down and just
    // finished reading blocks for gc and called do_write.
    rassert(state == state_ready
           || (state == state_shutting_down && gc_state.step() == gc_write));

    off64_t offset = gimme_a_new_offset();

    pm_serializer_data_blocks_written++;

    ls_buf_data_t *data = const_cast<ls_buf_data_t *>(reinterpret_cast<const ls_buf_data_t *>(buf_in) - 1);
    data->block_id = block_id;
    if (assign_new_block_sequence_id) {
        data->block_sequence_id = ++serializer->latest_block_sequence_id;
    }

    dbfile->write_async(offset, static_config->block_size().ser_value(), data, io_account, cb);

    return offset;
}

void data_block_manager_t::check_and_handle_empty_extent(unsigned int extent_id) {
    gc_entry *entry = entries.get(extent_id);
    if (!entry) {
        return; // The extent has already been deleted
    }

    if (entry->g_array.count() == static_config->blocks_per_extent() && entry->state != gc_entry::state_active) {
        /* Every block in the extent is now garbage. */
        switch (entry->state) {
            case gc_entry::state_reconstructing:
                unreachable("Marking something as garbage during startup.");

            case gc_entry::state_active:
                unreachable("We shouldn't have gotten here.");

            /* Remove from the young extent queue */
            case gc_entry::state_young:
                young_extent_queue.remove(entry);
                break;

            /* Remove from the priority queue */
            case gc_entry::state_old:
                gc_pq.remove(entry->our_pq_entry);
                gc_stats.old_total_blocks -= static_config->blocks_per_extent();
                gc_stats.old_garbage_blocks -= static_config->blocks_per_extent();
                break;

            /* Notify the GC that the extent got released during GC */
            case gc_entry::state_in_gc:
                rassert(gc_state.current_entry == entry);
                gc_state.current_entry = NULL;
                break;
            default:
                unreachable();
        }

        pm_serializer_data_extents_reclaimed++;
        entry->destroy();
        entries.set(extent_id, NULL);

    } else if (entry->state == gc_entry::state_old) {
        entry->our_pq_entry->update();
    }
}

file_account_t *data_block_manager_t::choose_gc_io_account() {
    // Start going into high priority as soon as the garbage ratio is more than
    // 2% above the configured goal.
    // The idea is that we use the nice i/o account whenever possible, except
    // if it proves insufficient to maintain an acceptable garbage ratio, in
    // which case we switch over to the high priority account until the situation
    // has improved.

    // This means that we can end up oscillating between both accounts, which
    // is probably fine. TODO: Make sure it actually is in practice!
    if (garbage_ratio() > dynamic_config->gc_high_ratio * 1.02f) {
        return gc_io_account_high.get();
    } else {
        return gc_io_account_nice.get();
    }
}

void data_block_manager_t::check_and_handle_empty_extent_later(unsigned int extent_id) {
    potentially_empty_extents.push_back(extent_id);
}

void data_block_manager_t::check_and_handle_outstanding_empty_extents() {
#ifndef NDEBUG
    if (potentially_empty_extents.size() != 0) {
        debugf("We got some potentially empty extents, yo! %zu of them.\n", potentially_empty_extents.size());
    }
    // Just have a brutal check to see if this case ever really
    // happens.
    rassert(potentially_empty_extents.size() == 0);
#endif
    for (size_t i = 0; i < potentially_empty_extents.size(); ++i) {
        check_and_handle_empty_extent(potentially_empty_extents[i]);
    }

    potentially_empty_extents.clear();
}

void data_block_manager_t::mark_garbage(off64_t offset) {
    unsigned int extent_id = static_config->extent_index(offset);
    unsigned int block_id = static_config->block_index(offset);
    
    gc_entry *entry = entries.get(extent_id);
    rassert(entry->i_array[block_id] == 1, "with block_id = %u", block_id);
    rassert(entry->g_array[block_id] == 0, "with block_id = %u", block_id);
    entry->i_array.set(block_id, 0);
    entry->update_g_array(block_id);
    
    rassert(entry->g_array.size() == static_config->blocks_per_extent());

    // Add to old garbage count if we have toggled the g_array bit (works because of the g_array[block_id] == 0 assertion above)
    if (entry->state == gc_entry::state_old && entry->g_array[block_id]) {
        gc_stats.old_garbage_blocks++;
    }
    
    check_and_handle_empty_extent(extent_id);
    /* We handle outstanding cleanup work now */
    check_and_handle_outstanding_empty_extents();
}

void data_block_manager_t::mark_token_live(off64_t offset) {
    unsigned int extent_id = static_config->extent_index(offset);
    unsigned int block_id = static_config->block_index(offset);

    gc_entry *entry = entries.get(extent_id);
    entry->t_array.set(block_id, 1);
    entry->update_g_array(block_id);
}

void data_block_manager_t::mark_token_garbage(off64_t offset) {
    unsigned int extent_id = static_config->extent_index(offset);
    unsigned int block_id = static_config->block_index(offset);

    gc_entry *entry = entries.get(extent_id);
    rassert(entry->t_array[block_id] == 1);
    rassert(entry->g_array[block_id] == 0);
    entry->t_array.set(block_id, 0);
    entry->update_g_array(block_id);

    rassert(entry->g_array.size() == static_config->blocks_per_extent());

    // Add to old garbage count if we have toggled the g_array bit (works because of the g_array[block_id] == 0 assertion above)
    if (entry->state == gc_entry::state_old && entry->g_array[block_id]) {
        gc_stats.old_garbage_blocks++;
    }

    // We delay this check as we don't want to interfere with active extent manager transactions
    // TODO: Maybe change how stuff works to make delaying this unnecessary?
    check_and_handle_empty_extent_later(extent_id);
}

void data_block_manager_t::start_gc() {
    if (gc_state.step() == gc_ready) run_gc();
}

data_block_manager_t::gc_writer_t::gc_writer_t(gc_write_t *writes, int num_writes, data_block_manager_t *_parent)
    : done(num_writes == 0), parent(_parent) {
    if (!done) {
        coro_t::spawn(boost::bind(&data_block_manager_t::gc_writer_t::write_gcs, this, writes, num_writes));
    }
}

struct block_write_cond_t : public cond_t, public iocallback_t {
    void on_io_complete() {
        pulse();
    }
};
void data_block_manager_t::gc_writer_t::write_gcs(gc_write_t* writes, int num_writes) {
    std::vector<block_write_cond_t*> block_write_conds;
    block_write_conds.reserve(num_writes);

    extent_manager_t::transaction_t *em_trx = parent->serializer->extent_manager->begin_transaction();
    // Step 1: Write buffers to disk and assemble index operations
    for (int i = 0; i < num_writes; ++i) {
        block_write_conds.push_back(new block_write_cond_t());

        const ls_buf_data_t *data = static_cast<const ls_buf_data_t *>(writes[i].buf) - 1;

        // the "false" argument indicates that we do not with to assign a new block sequence id
        writes[i].new_offset = parent->write(writes[i].buf, data->block_id, false, parent->choose_gc_io_account(), block_write_conds.back());
    }
    parent->serializer->extent_manager->end_transaction(em_trx);
    parent->serializer->extent_manager->commit_transaction(em_trx);

    // Step 2: Wait on all writes to finish
    for (size_t i = 0; i < block_write_conds.size(); ++i) {
        block_write_conds[i]->wait();
        delete block_write_conds[i];
    }

    // We just did a blocking operation, so recheck if we can access the current entry.
    if (parent->gc_state.current_entry != NULL) {

        std::vector<index_write_op_t> index_write_ops;

        // Step 3: Figure out index ops.  It's important that we do this
        // now, right before the index_write, so that the updates to the
        // index are done atomically.
        {
            ASSERT_NO_CORO_WAITING;

            for (int i = 0; i < num_writes; ++i) {
                unsigned int block_id = parent->static_config->block_index(writes[i].old_offset);

                if (parent->gc_state.current_entry->i_array[block_id]) {
                    const ls_buf_data_t *data = static_cast<const ls_buf_data_t *>(writes[i].buf) - 1;
                    boost::intrusive_ptr<ls_block_token_pointee_t> token = parent->serializer->generate_block_token(writes[i].new_offset);
                    index_write_ops.push_back(index_write_op_t(data->block_id, to_standard_block_token(data->block_id, token)));
                }

                // (If we don't have an i_array entry, the block is referenced
                // by a non-negative number of tokens only.  These get tokens
                // remapped later.)
            }

            // Step 4A: Remap tokens to new offsets.  It is important
            // that we do this _before_ calling index_write.
            // Otherwise, the token_offset map would still point to
            // the extent we're gcing.  Then somebody could do an
            // index_write after our index_write starts but before it
            // returns in Step 4 below, resulting in i_array entries
            // that point to the current entry.  This should empty out
            // all the t_array bits.
            for (int i = 0; i < num_writes; ++i) {
                parent->serializer->remap_block_to_new_offset(writes[i].old_offset, writes[i].new_offset);
            }

        }

        // Step 4B: Commit the transaction to the serializer, emptying
        // out all the i_array bits.
        parent->serializer->index_write(index_write_ops, parent->choose_gc_io_account());

        ASSERT_NO_CORO_WAITING;

        index_write_ops.clear();  // cleanup index_write_ops under the watchful eyes of ASSERT_NO_CORO_WAITING
    }

    {
        ASSERT_NO_CORO_WAITING;

        // Step 5: Call parent
        done = true;
        parent->on_gc_write_done();
    }

    // Step 6: Delete us
    delete this;
}

void data_block_manager_t::on_gc_write_done() {

    // Process GC data changes which have been caused by the tokens
    extent_manager_t::transaction_t *em_trx = serializer->extent_manager->begin_transaction();
    check_and_handle_outstanding_empty_extents();
    serializer->extent_manager->end_transaction(em_trx);
    serializer->extent_manager->commit_transaction(em_trx);

    // Continue GC
    run_gc();
}

void data_block_manager_t::run_gc() {
    // TODO: Convert this to a coroutine!
    bool run_again = true;
    while (run_again) {
        run_again = false;
        switch (gc_state.step()) {
            case gc_ready: {
                if (gc_pq.empty() || !should_we_keep_gcing(*gc_pq.peak())) {
                    return;
                }

                pm_serializer_data_extents_gced++;

                /* grab the entry */
                gc_state.current_entry = gc_pq.pop();
                gc_state.current_entry->our_pq_entry = NULL;

                rassert(gc_state.current_entry->state == gc_entry::state_old);
                gc_state.current_entry->state = gc_entry::state_in_gc;
                gc_stats.old_garbage_blocks -= gc_state.current_entry->g_array.count();
                gc_stats.old_total_blocks -= static_config->blocks_per_extent();

                /* read all the live data into buffers */

                /* make sure the read callback knows who we are */
                gc_state.gc_read_callback.parent = this;

                rassert(gc_state.refcount == 0);

                // read_async can call the callback immediately, which
                // will then decrement the refcount (and that's all it
                // will do if the refcount is not zero).  So we
                // increment the refcount here and set the step to
                // gc_read, before any calls to read_async.
                gc_state.refcount++;

                gc_state.set_step(gc_read);
#ifndef NDEBUG
                bool some_read_happened = false;
#endif
                for (unsigned int i = 0, bpe = static_config->blocks_per_extent(); i < bpe; i++) {
                    if (!gc_state.current_entry->g_array[i]) {
                        dbfile->read_async(gc_state.current_entry->offset + (i * static_config->block_size().ser_value()),
                                           static_config->block_size().ser_value(),
                                           gc_state.gc_blocks + (i * static_config->block_size().ser_value()),
                                           choose_gc_io_account(),
                                           &(gc_state.gc_read_callback));
#ifndef NDEBUG
                        some_read_happened = true;
#endif
                        gc_state.refcount++;
                    }
                }

                rassert(some_read_happened);

                // Fall through to the gc_read case, where we
                // decrement the refcount we incremented before the
                // for loop.
            }
            case gc_read: {
                gc_state.refcount--;
                if (gc_state.refcount > 0) {
                    /* We got a block, but there are still more to go */
                    break;
                }

                /* If other forces cause all of the blocks in the extent to become garbage
                before we even finish GCing it, they will set current_entry to NULL. */
                if (gc_state.current_entry == NULL) {
                    gc_state.set_step(gc_ready);
                    break;
                }

                /* an array to put our writes in */
#ifndef NDEBUG
                int num_writes = static_config->blocks_per_extent() - gc_state.current_entry->g_array.count();
#endif

                gc_writes.clear();
                for (unsigned int i = 0; i < static_config->blocks_per_extent(); i++) {

                    /* We re-check the bit array here in case a write came in for one of the
                    blocks we are GCing. We wouldn't want to overwrite the new valid data with
                    out-of-date data. */
                    if (gc_state.current_entry->g_array[i]) continue;

                    char *block = gc_state.gc_blocks + i * static_config->block_size().ser_value();
                    const off64_t block_offset = gc_state.current_entry->offset + (i * static_config->block_size().ser_value());
                    block_id_t id;
                    // The block is either referenced by an index or by a token (or both)
                    if (gc_state.current_entry->i_array[i]) {
                        id = (reinterpret_cast<ls_buf_data_t *>(block))->block_id;
                        rassert(id != NULL_BLOCK_ID);
                    } else {
                        id = NULL_BLOCK_ID;
                    }
                    void *data = block + sizeof(ls_buf_data_t);

                    gc_writes.push_back(gc_write_t(id, data, block_offset));
                }

                rassert(gc_writes.size() == (size_t)num_writes);

                gc_state.set_step(gc_write);

                /* schedule the write */
                gc_writer_t *gc_writer = new gc_writer_t(gc_writes.data(), gc_writes.size(), this);
                if (!gc_writer->done) break;
            }

            case gc_write:
                mark_unyoung_entries(); //We need to do this here so that we don't get stuck on the GC treadmill
                /* Our write should have forced all of the blocks in the extent to become garbage,
                which should have caused the extent to be released and gc_state.current_entry to
                become NULL. */

                rassert(gc_state.current_entry == NULL, "%zd garbage blocks left on the extent, %zd i_array blocks, %zd t_array blocks.\n", gc_state.current_entry->g_array.count(), gc_state.current_entry->i_array.count(), gc_state.current_entry->t_array.count());
                
                rassert(gc_state.refcount == 0);

                gc_state.set_step(gc_ready);

                if(state == state_shutting_down) {
                    actually_shutdown();
                    return;
                }

                run_again = true;   // We might want to start another GC round
                break;
                
            case gc_reconstruct:
            default:
                unreachable("Unknown gc_step");
        }
    }
}

void data_block_manager_t::prepare_metablock(metablock_mixin_t *metablock) {
    rassert(state == state_ready || state == state_shutting_down);
    
    for (int i = 0; i < MAX_ACTIVE_DATA_EXTENTS; i++) {
        if (active_extents[i]) {
            metablock->active_extents[i] = active_extents[i]->offset;
            metablock->blocks_in_active_extent[i] = blocks_in_active_extent[i];
        } else {
            metablock->active_extents[i] = NULL_OFFSET;
            metablock->blocks_in_active_extent[i] = 0;
        }
    }
}

bool data_block_manager_t::shutdown(shutdown_callback_t *cb) {
    rassert(cb);
    rassert(state == state_ready);
    state = state_shutting_down;

    if(gc_state.step() != gc_ready) {
        shutdown_callback = cb;
        return false;
    } else {
        shutdown_callback = NULL;
        actually_shutdown();
        return true;
    }
}

void data_block_manager_t::actually_shutdown() {
    rassert(state == state_shutting_down);
    state = state_shut_down;

    rassert(!reconstructed_extents.head());
    
    for (unsigned int i = 0; i < dynamic_config->num_active_data_extents; i++) {
        if (active_extents[i]) {
            delete active_extents[i];
            active_extents[i] = NULL;
        }
    }
    
    while (gc_entry *entry = young_extent_queue.head()) {
        young_extent_queue.remove(entry);
        delete entry;
    }
    
    while (!gc_pq.empty()) {
        delete gc_pq.pop();
    }
    
    if (shutdown_callback) {
        shutdown_callback->on_datablock_manager_shutdown();
    }
}

off64_t data_block_manager_t::gimme_a_new_offset() {
    /* Start a new extent if necessary */
    
    if (!active_extents[next_active_extent]) {
        active_extents[next_active_extent] = new gc_entry(this);
        active_extents[next_active_extent]->state = gc_entry::state_active;
        blocks_in_active_extent[next_active_extent] = 0;
        
        pm_serializer_data_extents_allocated++;
    }
    
    /* Put the block into the chosen extent */
    
    rassert(active_extents[next_active_extent]->state == gc_entry::state_active);
    rassert(active_extents[next_active_extent]->g_array.count() > 0);
    rassert(blocks_in_active_extent[next_active_extent] < static_config->blocks_per_extent());

    off64_t offset = active_extents[next_active_extent]->offset + blocks_in_active_extent[next_active_extent] * static_config->block_size().ser_value();
    active_extents[next_active_extent]->was_written = true;

    rassert(active_extents[next_active_extent]->g_array[blocks_in_active_extent[next_active_extent]]);

    blocks_in_active_extent[next_active_extent]++;
    
    /* Deactivate the extent if necessary */
    
    if (blocks_in_active_extent[next_active_extent] == static_config->blocks_per_extent()) {
        rassert(active_extents[next_active_extent]->g_array.count() < static_config->blocks_per_extent());
        active_extents[next_active_extent]->state = gc_entry::state_young;
        young_extent_queue.push_back(active_extents[next_active_extent]);
        mark_unyoung_entries();
        active_extents[next_active_extent] = NULL;
    }
    
    /* Move along to the next extent. This logic is kind of weird because it needs to handle the
    case where we have just started up and we still have active extents open from a previous run,
    but the value of num_active_data_extents was higher on that previous run and so there are active
    data extents that occupy slots in active_extents that are higher than our current value of
    num_active_data_extents. The way we handle this case is by continuing to visit those slots until
    the data extents fill up and are deactivated, but then not visiting those slots any more. */
    
    do {
        next_active_extent = (next_active_extent + 1) % MAX_ACTIVE_DATA_EXTENTS;
    } while (next_active_extent >= dynamic_config->num_active_data_extents &&
             !active_extents[next_active_extent]);
    
    return offset;
}

// Looks at young_extent_queue and pops things off the queue that are
// no longer deemed young, putting them on the priority queue.
void data_block_manager_t::mark_unyoung_entries() {
    while (young_extent_queue.size() > GC_YOUNG_EXTENT_MAX_SIZE) {
        remove_last_unyoung_entry();
    }

    uint64_t current_time = current_microtime();

    while (young_extent_queue.head()
           && current_time - young_extent_queue.head()->timestamp > GC_YOUNG_EXTENT_TIMELIMIT_MICROS) {
        remove_last_unyoung_entry();
    }
}

// Pops young_extent_queue and puts it on the priority queue.
// Assumes young_extent_queue is not empty.
void data_block_manager_t::remove_last_unyoung_entry() {
    gc_entry *entry = young_extent_queue.head();
    young_extent_queue.remove(entry);
    
    rassert(entry->state == gc_entry::state_young);
    entry->state = gc_entry::state_old;
    
    entry->our_pq_entry = gc_pq.push(entry);
    
    gc_stats.old_total_blocks += static_config->blocks_per_extent();
    gc_stats.old_garbage_blocks += entry->g_array.count();
}


gc_entry::gc_entry(data_block_manager_t *_parent)
    : parent(_parent),
      offset(parent->extent_manager->gen_extent()),
      g_array(parent->static_config->blocks_per_extent()),
      t_array(parent->static_config->blocks_per_extent()),
      i_array(parent->static_config->blocks_per_extent()),
      timestamp(current_microtime()),
      was_written(false)
{
    rassert(parent->entries.get(offset / parent->extent_manager->extent_size) == NULL);
    parent->entries.set(offset / parent->extent_manager->extent_size, this);
    g_array.set();

    pm_serializer_data_extents++;
}

gc_entry::gc_entry(data_block_manager_t *_parent, off64_t _offset)
    : parent(_parent),
      offset(_offset),
      g_array(parent->static_config->blocks_per_extent()),
      t_array(parent->static_config->blocks_per_extent()),
      i_array(parent->static_config->blocks_per_extent()),
      timestamp(current_microtime()),
      was_written(false)
{
    parent->extent_manager->reserve_extent(offset);
    rassert(parent->entries.get(offset / parent->extent_manager->extent_size) == NULL);
    parent->entries.set(offset / parent->extent_manager->extent_size, this);
    g_array.set();

    pm_serializer_data_extents++;
}

gc_entry::~gc_entry() {
    rassert(parent->entries.get(offset / parent->extent_manager->extent_size) == this);
    parent->entries.set(offset / parent->extent_manager->extent_size, NULL);

    pm_serializer_data_extents--;
}

void gc_entry::destroy() {
    parent->extent_manager->release_extent(offset);
    delete this;
}

#ifndef NDEBUG
void gc_entry::print() {
    debugf("gc_entry:\n");
    debugf("offset: %ld\n", offset);
    for (unsigned int i = 0; i < g_array.size(); i++)
        debugf("%.8x:\t%d\n", (unsigned int) (offset + (i * DEVICE_BLOCK_SIZE)), g_array.test(i));
    debugf("\n");
    debugf("\n");
}
#endif





/* functions for gc structures */

// Answers the following question: We're in the middle of gc'ing, and
// look, it's the next largest entry.  Should we keep gc'ing?  Returns
// false when the entry is active or young, or when its garbage ratio
// is lower than GC_THRESHOLD_RATIO_*.
bool data_block_manager_t::should_we_keep_gcing(UNUSED const gc_entry& entry) const {
    return !gc_state.should_be_stopped && garbage_ratio() > dynamic_config->gc_low_ratio;
}

// Answers the following question: Do we want to bother gc'ing?
// Returns true when our garbage_ratio is greater than
// GC_THRESHOLD_RATIO_*.
bool data_block_manager_t::do_we_want_to_start_gcing() const {
    return !gc_state.should_be_stopped && garbage_ratio() > dynamic_config->gc_high_ratio;
}

/* !< is x less than y */
bool gc_entry_less::operator() (const gc_entry *x, const gc_entry *y) {
    return x->g_array.count() < y->g_array.count();
}

/****************
 *Stat functions*
 ****************/

float data_block_manager_t::garbage_ratio() const {
    if (gc_stats.old_total_blocks.get() == 0) {
        return 0.0;
    } else {
        return (float) gc_stats.old_garbage_blocks.get() / ((float) gc_stats.old_total_blocks.get() + extent_manager->held_extents() * static_config->blocks_per_extent());
    }
}

bool data_block_manager_t::disable_gc(gc_disable_callback_t *cb) {
    // We _always_ call the callback!

    rassert(gc_state.gc_disable_callback == NULL);
    gc_state.should_be_stopped = true;

    if (gc_state.step() != gc_ready && gc_state.step() != gc_reconstruct) {
        gc_state.gc_disable_callback = cb;
        return false;
    } else {
        cb->on_gc_disabled();
        return true;
    }
}

void data_block_manager_t::enable_gc() {
    gc_state.should_be_stopped = false;
}


void data_block_manager_t::gc_stat_t::operator++(int) {
    val++;
    (*perfmon)++;
}

void data_block_manager_t::gc_stat_t::operator+=(int64_t num) {
    val += num;
    *perfmon += num;
}

void data_block_manager_t::gc_stat_t::operator--(int) {
    val--;
    perfmon--;
}

void data_block_manager_t::gc_stat_t::operator-=(int64_t num) {
    val -= num;
    *perfmon -= num;
}

data_block_manager_t::gc_stats_t::gc_stats_t()
    : old_total_blocks(&pm_serializer_old_total_blocks), old_garbage_blocks(&pm_serializer_old_garbage_blocks) { }
