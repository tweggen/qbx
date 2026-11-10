#pragma once

// THE RT PUBLICATION HANDSHAKE (QBX-287, factored out by QBX-297).
//
// A realtime reader that must use an object a control thread may REPLACE reads
// it through a raw `std::atomic<T*>`, never a `shared_ptr`: libstdc++ answers
// `std::atomic_load` on a `shared_ptr` with a pooled mutex (`_Sp_locker`), and
// the reader's owning copy could be the last reference, so the object would be
// destroyed on the RT. A raw pointer is lock-free and owns nothing, which moves
// the lifetime question to the writer: it may release the object it replaced
// only once no RT block can still be using it. This counter answers that.
//
//   RT, once per block:              Writer, per replacement:
//     twRtQuiescence::Block b(q);      ptr.store(next, seq_cst);
//     T *p = ptr.load(seq_cst);        if (q.awaitQuiescent()) release(old);
//     ... use p, never past b ...      else                     park(old);
//
// The count is ODD while a block is in flight. Block's increment and the
// reader's load are seq_cst, and so are the writer's store and its first load
// of the count, so in the single total order either the writer sees the block
// in flight (odd), or the block's load sees the new pointer -- never neither.
// An even count therefore proves that every block still to come reads `next`;
// an odd one names the block that may hold `old`, and that block is over once
// the count moves. The wait is BOUNDED: a reader that has not finished one
// block by then is stalled, and the writer must park `old` (keep it alive)
// rather than free it under that block, or hang the control thread.
//
// What each owner does with a parked object is its own policy (AudioEngine
// releases parked roots inline; twSpeaker must destroy engines outside every
// lock), so only the handshake lives here. One reader per counter: the count
// brackets ONE thread's blocks, and two concurrent readers would make its
// parity meaningless.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

class twRtQuiescence {
public:
    // A bound many device blocks long: only a stalled reader runs into it.
    static constexpr std::chrono::milliseconds kDefaultBound{ 250 };

    // RT side: brackets one block. Construct it BEFORE the pointer is loaded and
    // let it die only after the block's last use of that pointer.
    class Block {
    public:
        explicit Block( twRtQuiescence &q ) : q_( q )
        { q_.seq_.fetch_add( 1, std::memory_order_seq_cst ); }
        ~Block() { q_.seq_.fetch_add( 1, std::memory_order_release ); }
        Block( const Block & ) = delete;
        Block &operator=( const Block & ) = delete;
    private:
        twRtQuiescence &q_;
    };

    // Writer side, right AFTER the seq_cst store of the replacement. True once
    // no block that may have read the old pointer is still in flight; false if
    // `bound` ran out first (the reader is stalled: park, do not release).
    bool awaitQuiescent( std::chrono::milliseconds bound = kDefaultBound ) const
    {
        const std::uint32_t inFlight = seq_.load( std::memory_order_seq_cst );
        if( ( inFlight & 1u ) == 0 ) return true;
        const auto deadline = std::chrono::steady_clock::now() + bound;
        while( seq_.load( std::memory_order_acquire ) == inFlight ) {
            if( std::chrono::steady_clock::now() >= deadline ) return false;
            std::this_thread::yield();
        }
        return true;
    }

    // Test seam: true while a block is in flight.
    bool inBlockForTest() const
    { return ( seq_.load( std::memory_order_acquire ) & 1u ) != 0; }

private:
    std::atomic<std::uint32_t> seq_{ 0 };
};
