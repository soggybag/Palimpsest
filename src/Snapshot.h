#pragma once
#include <cstddef>
#include <cstdint>
#include "LoopBuffer.h"

// Block copy-on-write layer shared by Overdub, Substitute, and Undo.
//
// The loop buffer is divided into fixed blocks. Before a destructive write first
// touches a block within a "take", that block is copied into a pool slot. Ending
// a take either COMMITS (the take's snapshots become the one-deep undo set) or
// REVERTS (snapshots are restored and discarded, and any prior committed undo
// survives).
//
// Undo swaps live <-> snapshot for the committed set; Undo again = redo.
//
// Pool exhausted -> the ring allocator drops the oldest un-kept snapshot and
// that region becomes un-undoable. No allocation, no blocking.
class Snapshot
{
  public:
    static constexpr uint32_t kBlock     = 4096;   // ~85 ms @ 48 kHz
    static constexpr uint16_t kNone      = 0xFFFF;
    static constexpr int      kMaxBlocks = 4700;   // covers 19.2 M-sample buffer
    static constexpr int      kMaxSlots  = 2048;   // 16 MB / 2 B / kBlock

    void Init(int16_t* pool, size_t pool_samps, size_t buf_samps)
    {
        pool_       = pool;
        n_slots_    = (int)(pool_samps / kBlock);
        if(n_slots_ > kMaxSlots)
            n_slots_ = kMaxSlots;
        buf_samps_  = buf_samps;
        Reset();
    }

    // Clear all bookkeeping (pool/size stay as configured by Init). Call this
    // whenever the live buffer's content is replaced out from under the block
    // map by something other than Touch/Commit/Revert -- e.g. Crop & Tile -- so
    // stale block->slot mappings can't be misapplied to unrelated new content.
    void Reset()
    {
        next_slot_  = 0;
        take_n_     = 0;
        undo_n_     = 0;
        undone_     = false;
        for(int i = 0; i < kMaxBlocks; i++)
        {
            take_slot_[i] = kNone;
            undo_slot_[i] = kNone;
        }
        for(int i = 0; i < kMaxSlots; i++)
        {
            slot_owner_[i] = kNone;
            slot_kept_[i]  = false;
        }
    }

    bool CanUndo() const { return undo_n_ > 0; }
    bool IsUndone() const { return undone_; }

    // Start a fresh destructive take (rising edge of Overdub / Substitute).
    void BeginTake()
    {
        for(int i = 0; i < take_n_; i++)
        {
            uint16_t s = take_slot_[take_blk_[i]];
            if(s != kNone)
            {
                slot_owner_[s]        = kNone;
                take_slot_[take_blk_[i]] = kNone;
            }
        }
        take_n_ = 0;
    }

    // Call before a destructive write to `idx`.
    void Touch(const LoopBuffer& buf, size_t idx)
    {
        if(idx >= buf_samps_)
            return;
        uint32_t blk = (uint32_t)(idx / kBlock);
        if(blk >= (uint32_t)kMaxBlocks || take_slot_[blk] != kNone)
            return;
        uint16_t s = Alloc();
        if(s == kNone)
            return; // pool fully kept — region becomes un-undoable
        size_t base = (size_t)blk * kBlock;
        size_t n    = kBlock;
        if(base + n > buf_samps_)
            n = buf_samps_ - base;
        int16_t* dst = pool_ + (size_t)s * kBlock;
        for(size_t i = 0; i < n; i++)
            dst[i] = buf.Raw(base + i);
        take_slot_[blk] = s;
        slot_owner_[s]  = (uint16_t)blk;
        if(take_n_ < kMaxSlots)
            take_blk_[take_n_++] = (uint16_t)blk;
    }

    // End the take, keeping the changes. The take's snapshots replace the
    // previous undo set.
    void Commit()
    {
        for(int i = 0; i < undo_n_; i++)
        {
            uint16_t s = undo_slot_[undo_blk_[i]];
            if(s != kNone)
            {
                slot_kept_[s]           = false;
                slot_owner_[s]          = kNone;
                undo_slot_[undo_blk_[i]] = kNone;
            }
        }
        undo_n_ = 0;
        for(int i = 0; i < take_n_; i++)
        {
            uint16_t blk = take_blk_[i];
            uint16_t s   = take_slot_[blk];
            take_slot_[blk] = kNone;
            if(s == kNone)
                continue;
            slot_kept_[s]   = true;
            undo_slot_[blk] = s;
            undo_blk_[undo_n_++] = blk;
        }
        take_n_ = 0;
        undone_ = false;
    }

    // End the take, restoring the buffer. Prior undo set is untouched.
    void RevertTake(LoopBuffer& buf)
    {
        for(int i = 0; i < take_n_; i++)
        {
            uint16_t blk = take_blk_[i];
            uint16_t s   = take_slot_[blk];
            if(s == kNone)
                continue;
            RestoreBlock(buf, blk, s);
            slot_owner_[s]  = kNone;
            take_slot_[blk] = kNone;
        }
        take_n_ = 0;
    }

    // Toggle the committed undo set (undo <-> redo).
    void UndoRedo(LoopBuffer& buf)
    {
        for(int i = 0; i < undo_n_; i++)
            SwapBlock(buf, undo_blk_[i], undo_slot_[undo_blk_[i]]);
        undone_ = !undone_;
    }

  private:
    uint16_t Alloc()
    {
        for(int tries = 0; tries < n_slots_; tries++)
        {
            uint16_t s = next_slot_;
            next_slot_ = (uint16_t)((next_slot_ + 1) % n_slots_);
            if(slot_kept_[s])
                continue;
            uint16_t owner = slot_owner_[s];
            if(owner != kNone)
            {
                // evict: whichever map points here loses its snapshot
                if(take_slot_[owner] == s)
                    take_slot_[owner] = kNone;
                if(undo_slot_[owner] == s)
                    undo_slot_[owner] = kNone;
            }
            return s;
        }
        return kNone;
    }

    void RestoreBlock(LoopBuffer& buf, uint16_t blk, uint16_t s)
    {
        size_t base = (size_t)blk * kBlock;
        size_t n    = kBlock;
        if(base + n > buf_samps_)
            n = buf_samps_ - base;
        const int16_t* src = pool_ + (size_t)s * kBlock;
        for(size_t i = 0; i < n; i++)
            buf.SetRaw(base + i, src[i]);
    }

    void SwapBlock(LoopBuffer& buf, uint16_t blk, uint16_t s)
    {
        size_t base = (size_t)blk * kBlock;
        size_t n    = kBlock;
        if(base + n > buf_samps_)
            n = buf_samps_ - base;
        int16_t* pp = pool_ + (size_t)s * kBlock;
        for(size_t i = 0; i < n; i++)
        {
            int16_t t = buf.Raw(base + i);
            buf.SetRaw(base + i, pp[i]);
            pp[i] = t;
        }
    }

    int16_t* pool_      = nullptr;
    int      n_slots_   = 0;
    size_t   buf_samps_ = 0;
    uint16_t next_slot_ = 0;

    uint16_t take_slot_[kMaxBlocks];
    uint16_t undo_slot_[kMaxBlocks];
    uint16_t slot_owner_[kMaxSlots];
    bool     slot_kept_[kMaxSlots];

    uint16_t take_blk_[kMaxSlots];
    uint16_t undo_blk_[kMaxSlots];
    int      take_n_ = 0;
    int      undo_n_ = 0;
    bool     undone_ = false;
};
