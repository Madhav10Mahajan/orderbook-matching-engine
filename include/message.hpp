// One input event for the engine: what a replay file line or a generated
// order-flow event turns into before it reaches the book.
#pragma once

#include "types.hpp"

namespace ob {

enum class MsgType : char {
    Add = 'A',     // limit order (with time-in-force)
    Market = 'M',  // market order
    Cancel = 'C',
    Modify = 'X',  // new price + new remaining qty
};

struct Message {
    MsgType type = MsgType::Add;
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity qty = 0;
    TimeInForce tif = TimeInForce::GTC;
};

// Sends one message to any book with the common interface.
template <class Book>
ExecResult apply(Book& book, const Message& m) {
    switch (m.type) {
        case MsgType::Add: return book.add_limit(m.id, m.side, m.price, m.qty, m.tif);
        case MsgType::Market: return book.add_market(m.id, m.side, m.qty);
        case MsgType::Modify: return book.modify(m.id, m.price, m.qty);
        case MsgType::Cancel: {
            ExecResult r;
            r.status = book.cancel(m.id);
            return r;
        }
    }
    return {};
}

}  // namespace ob
