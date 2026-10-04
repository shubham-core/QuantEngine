// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2024 QuantEngine Authors
//
// This file is part of QuantEngine.
// QuantEngine is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// For commercial licensing without the AGPL restrictions, contact:
//   commercial@quantengine.io

#include <pybind11/chrono.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "quantengine/core/events.hpp"
#include "quantengine/core/order.hpp"
#include "quantengine/core/trade.hpp"
#include "quantengine/core/types.hpp"
#include "quantengine/core/version.hpp"
#include "quantengine/engine/canonical_state.hpp"
#include "quantengine/engine/generic_matching_engine.hpp"
#include "quantengine/engine/invariants.hpp"
#include "quantengine/engine/replay.hpp"
#include "quantengine/optimized/optimized_order_book.hpp"
#include "quantengine/reference/reference_order_book.hpp"

namespace py = pybind11;
using namespace quantengine;
using namespace quantengine::core;
using namespace quantengine::engine;

// Struct for high-performance batch submissions
struct BatchOrder {
    OrderId order_id{0};
    Side side{Side::Buy};
    PriceTicks price{0};
    Quantity quantity{0};
};

template <typename EngineType>
static void bind_engine_methods(py::class_<EngineType>& cls) {
    cls.def("submit_order", &EngineType::submit_order, py::arg("order_id"), py::arg("side"),
            py::arg("price"), py::arg("quantity"),
            "Submit a new limit order to the matching engine")
        .def("cancel_order", &EngineType::cancel_order, py::arg("order_id"),
             "Cancel a resting order by its ID")
        .def("modify_order", &EngineType::modify_order, py::arg("order_id"), py::arg("new_price"),
             py::arg("new_quantity"),
             "Modify a resting order (cancel + re-insert with new price/qty)")
        .def("process_command", &EngineType::process_command, py::arg("command"),
             "Process a polymorphic order command (Create, Cancel, or Modify)")
        .def("reset", &EngineType::reset, "Reset engine and book to initial empty state")
        .def("current_sequence", &EngineType::current_sequence,
             "Get the most recent sequence number assigned by the engine")
        .def("current_trade_id", &EngineType::current_trade_id,
             "Get the most recent trade ID assigned by the engine")
        .def(
            "best_bid_price",
            [](const EngineType& self) -> std::optional<PriceTicks> {
                return self.book().best_bid_price();
            },
            "Get the highest resting bid price (or None if empty)")
        .def(
            "best_ask_price",
            [](const EngineType& self) -> std::optional<PriceTicks> {
                return self.book().best_ask_price();
            },
            "Get the lowest resting ask price (or None if empty)")
        .def(
            "best_bid_quantity",
            [](const EngineType& self) -> std::optional<Quantity> {
                return self.book().best_bid_quantity();
            },
            "Get the aggregate quantity at the best bid (or None if empty)")
        .def(
            "best_ask_quantity",
            [](const EngineType& self) -> std::optional<Quantity> {
                return self.book().best_ask_quantity();
            },
            "Get the aggregate quantity at the best ask (or None if empty)")
        .def(
            "total_bid_volume",
            [](const EngineType& self) -> Quantity { return self.book().total_bid_volume(); },
            "Get the total resting volume on the bid side")
        .def(
            "total_ask_volume",
            [](const EngineType& self) -> Quantity { return self.book().total_ask_volume(); },
            "Get the total resting volume on the ask side")
        .def(
            "total_orders",
            [](const EngineType& self) -> std::size_t { return self.book().total_orders(); },
            "Get the total count of active resting orders in the book")
        .def(
            "is_empty", [](const EngineType& self) -> bool { return self.book().is_empty(); },
            "Check if the order book is completely empty")
        .def(
            "get_bids",
            [](const EngineType& self, std::size_t max_levels) {
                return self.book().get_bids(max_levels);
            },
            py::arg("max_levels") = 0, "Get sorted bid depth levels (descending by price)")
        .def(
            "get_asks",
            [](const EngineType& self, std::size_t max_levels) {
                return self.book().get_asks(max_levels);
            },
            py::arg("max_levels") = 0, "Get sorted ask depth levels (ascending by price)")
        .def(
            "canonical_hash",
            [](const EngineType& self) -> std::uint64_t {
                return CanonicalState::compute_hash(self);
            },
            "Compute 64-bit FNV-1a canonical binary state hash")
        .def(
            "serialize_state",
            [](const EngineType& self) -> py::bytes {
                const auto bytes = CanonicalState::serialize(self);
                return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            },
            "Serialize canonical binary engine state")
        .def(
            "audit",
            [](const EngineType& self) -> std::pair<bool, std::string> {
                const auto res = InvariantAuditor::audit(self);
                return {res.ok, res.error_message};
            },
            "Audit all internal invariant checks (uncrossed book, volume consistency, pool "
            "integrity)")
        .def(
            "submit_batch",
            [](EngineType& self, const py::iterable& batch) -> std::vector<ExecutionReport> {
                std::vector<BatchOrder> orders;
                for (const auto& item : batch) {
                    if (py::isinstance<BatchOrder>(item)) {
                        orders.push_back(item.cast<BatchOrder>());
                    } else if (py::isinstance<py::tuple>(item)) {
                        auto t = item.cast<py::tuple>();
                        if (t.size() != 4) {
                            throw std::invalid_argument(
                                "Tuple must contain (order_id, side, price, quantity)");
                        }
                        orders.push_back(BatchOrder{t[0].cast<OrderId>(), t[1].cast<Side>(),
                                                    t[2].cast<PriceTicks>(),
                                                    t[3].cast<Quantity>()});
                    } else {
                        throw std::invalid_argument(
                            "Batch items must be BatchOrder or (order_id, side, price, quantity) "
                            "tuple");
                    }
                }

                std::vector<ExecutionReport> reports;
                reports.reserve(orders.size());
                {
                    py::gil_scoped_release release;
                    for (const auto& ord : orders) {
                        reports.push_back(
                            self.submit_order(ord.order_id, ord.side, ord.price, ord.quantity));
                    }
                }
                return reports;
            },
            py::arg("orders"),
            "Submit a batch of orders (BatchOrder or tuples) releasing the Python GIL")
        .def(
            "process_commands_batch",
            [](EngineType& self,
               const std::vector<OrderCommand>& commands) -> std::vector<ExecutionReport> {
                std::vector<ExecutionReport> reports;
                reports.reserve(commands.size());
                {
                    py::gil_scoped_release release;
                    for (const auto& cmd : commands) {
                        reports.push_back(self.process_command(cmd));
                    }
                }
                return reports;
            },
            py::arg("commands"), "Process a batch of OrderCommands releasing the Python GIL");
}

PYBIND11_MODULE(quantengine, m) {
    m.doc() = "QuantEngine: High-Performance Deterministic Limit Order Matching Engine";
    m.attr("__version__") = get_version();

    // -----------------------------------------------------------------------
    // Core Enums
    // -----------------------------------------------------------------------
    py::enum_<Side>(m, "Side", "Order trading side (Buy / Sell)")
        .value("Buy", Side::Buy)
        .value("Sell", Side::Sell)
        .export_values();

    py::enum_<OrderStatus>(m, "OrderStatus", "Order lifecycle state")
        .value("New", OrderStatus::New)
        .value("PartiallyFilled", OrderStatus::PartiallyFilled)
        .value("Filled", OrderStatus::Filled)
        .value("Cancelled", OrderStatus::Cancelled)
        .value("Rejected", OrderStatus::Rejected)
        .value("Resting", OrderStatus::Resting)
        .export_values();

    py::enum_<OrderType>(m, "OrderType", "Order execution type")
        .value("Limit", OrderType::Limit)
        .export_values();

    py::enum_<RejectReason>(m, "RejectReason", "Order rejection cause")
        .value("None", RejectReason::None)
        .value("NoReject", RejectReason::None)
        .value("DuplicateOrderId", RejectReason::DuplicateOrderId)
        .value("UnknownOrder", RejectReason::UnknownOrder)
        .value("InvalidPrice", RejectReason::InvalidPrice)
        .value("InvalidQuantity", RejectReason::InvalidQuantity)
        .value("OrderAlreadyFilled", RejectReason::OrderAlreadyFilled)
        .value("OrderAlreadyCancelled", RejectReason::OrderAlreadyCancelled)
        .value("InvalidStateTransition", RejectReason::InvalidStateTransition)
        .export_values();

    // -----------------------------------------------------------------------
    // Core Data Structures
    // -----------------------------------------------------------------------
    py::class_<Order>(m, "Order", "Individual limit order record")
        .def(py::init<OrderId, Side, PriceTicks, Quantity, SeqNum>(), py::arg("order_id"),
             py::arg("side"), py::arg("price"), py::arg("quantity"), py::arg("sequence_number") = 0)
        .def_property_readonly("order_id", &Order::order_id)
        .def_property_readonly("side", &Order::side)
        .def_property_readonly("price", &Order::price)
        .def_property_readonly("initial_quantity", &Order::initial_quantity)
        .def_property_readonly("remaining_quantity", &Order::remaining_quantity)
        .def_property_readonly("filled_quantity", &Order::filled_quantity)
        .def_property_readonly("status", &Order::status)
        .def_property_readonly("sequence_number", &Order::sequence_number)
        .def("is_active", &Order::is_active)
        .def("is_terminal", &Order::is_terminal)
        .def("is_filled", [](const Order& o) { return o.status() == OrderStatus::Filled; })
        .def("__repr__", [](const Order& o) {
            std::ostringstream ss;
            ss << "<Order id=" << o.order_id() << " " << to_string(o.side()) << " "
               << o.remaining_quantity() << "/" << o.initial_quantity() << " @" << o.price()
               << " status=" << to_string(o.status()) << ">";
            return ss.str();
        });

    py::class_<Trade>(m, "Trade", "Execution record between maker and taker")
        .def(py::init<TradeId, OrderId, OrderId, Side, PriceTicks, Quantity, SeqNum>(),
             py::arg("trade_id") = 0, py::arg("maker_order_id") = 0, py::arg("taker_order_id") = 0,
             py::arg("maker_side") = Side::Buy, py::arg("price") = 0, py::arg("quantity") = 0,
             py::arg("sequence_number") = 0)
        .def_readwrite("trade_id", &Trade::trade_id)
        .def_readwrite("maker_order_id", &Trade::maker_order_id)
        .def_readwrite("taker_order_id", &Trade::taker_order_id)
        .def_readwrite("maker_side", &Trade::maker_side)
        .def_readwrite("price", &Trade::price)
        .def_readwrite("quantity", &Trade::quantity)
        .def_readwrite("sequence_number", &Trade::sequence_number)
        .def("__repr__", [](const Trade& t) {
            std::ostringstream ss;
            ss << "<Trade id=" << t.trade_id << " maker=" << t.maker_order_id
               << " taker=" << t.taker_order_id << " " << t.quantity << " @" << t.price
               << " seq=" << t.sequence_number << ">";
            return ss.str();
        });

    py::class_<ExecutionReport>(m, "ExecutionReport", "Order response report")
        .def_readwrite("order_id", &ExecutionReport::order_id)
        .def_readwrite("status", &ExecutionReport::status)
        .def_readwrite("reject_reason", &ExecutionReport::reject_reason)
        .def_readwrite("remaining_quantity", &ExecutionReport::remaining_quantity)
        .def_readwrite("filled_quantity", &ExecutionReport::filled_quantity)
        .def_readwrite("price", &ExecutionReport::price)
        .def_readwrite("side", &ExecutionReport::side)
        .def_readwrite("sequence_number", &ExecutionReport::sequence_number)
        .def_readwrite("trades", &ExecutionReport::trades)
        .def("__repr__", [](const ExecutionReport& r) {
            std::ostringstream ss;
            ss << "<ExecutionReport id=" << r.order_id << " status=" << to_string(r.status)
               << " rem=" << r.remaining_quantity << " fill=" << r.filled_quantity
               << " trades=" << r.trades.size() << ">";
            return ss.str();
        });

    py::class_<reference::LevelInfo>(m, "LevelInfo", "Price level aggregate depth")
        .def(py::init<PriceTicks, Quantity, std::size_t>(), py::arg("price") = 0,
             py::arg("total_quantity") = 0, py::arg("order_count") = 0)
        .def_readwrite("price", &reference::LevelInfo::price)
        .def_readwrite("total_quantity", &reference::LevelInfo::total_quantity)
        .def_readwrite("order_count", &reference::LevelInfo::order_count)
        .def("__repr__", [](const reference::LevelInfo& l) {
            std::ostringstream ss;
            ss << "<LevelInfo price=" << l.price << " qty=" << l.total_quantity
               << " orders=" << l.order_count << ">";
            return ss.str();
        });

    py::class_<BatchOrder>(m, "BatchOrder", "Batch order specification struct")
        .def(py::init<OrderId, Side, PriceTicks, Quantity>(), py::arg("order_id"), py::arg("side"),
             py::arg("price"), py::arg("quantity"))
        .def_readwrite("order_id", &BatchOrder::order_id)
        .def_readwrite("side", &BatchOrder::side)
        .def_readwrite("price", &BatchOrder::price)
        .def_readwrite("quantity", &BatchOrder::quantity)
        .def("__repr__", [](const BatchOrder& b) {
            std::ostringstream ss;
            ss << "<BatchOrder id=" << b.order_id << " " << to_string(b.side)
               << " price=" << b.price << " qty=" << b.quantity << ">";
            return ss.str();
        });

    // -----------------------------------------------------------------------
    // Command Objects
    // -----------------------------------------------------------------------
    py::class_<CreateOrderCommand>(m, "CreateOrderCommand")
        .def(py::init<OrderId, Side, PriceTicks, Quantity>(), py::arg("order_id"), py::arg("side"),
             py::arg("price"), py::arg("quantity"))
        .def_readwrite("order_id", &CreateOrderCommand::order_id)
        .def_readwrite("side", &CreateOrderCommand::side)
        .def_readwrite("price", &CreateOrderCommand::price)
        .def_readwrite("quantity", &CreateOrderCommand::quantity);

    py::class_<CancelOrderCommand>(m, "CancelOrderCommand")
        .def(py::init<OrderId>(), py::arg("order_id"))
        .def_readwrite("order_id", &CancelOrderCommand::order_id);

    py::class_<ModifyOrderCommand>(m, "ModifyOrderCommand")
        .def(py::init<OrderId, PriceTicks, Quantity>(), py::arg("order_id"), py::arg("new_price"),
             py::arg("new_quantity"))
        .def_readwrite("order_id", &ModifyOrderCommand::order_id)
        .def_readwrite("new_price", &ModifyOrderCommand::new_price)
        .def_readwrite("new_quantity", &ModifyOrderCommand::new_quantity);

    py::class_<OrderCommand>(m, "OrderCommand")
        .def(py::init<SeqNum, OrderCommandPayload>(), py::arg("sequence_number") = 0,
             py::arg("payload"))
        .def_readwrite("sequence_number", &OrderCommand::sequence_number)
        .def_readwrite("payload", &OrderCommand::payload);

    // -----------------------------------------------------------------------
    // Matching Engines
    // -----------------------------------------------------------------------
    py::class_<ReferenceMatchingEngine> ref_engine(
        m, "ReferenceMatchingEngine", "Reference STL-based limit order matching engine");
    ref_engine.def(py::init<>());
    bind_engine_methods(ref_engine);

    py::class_<OptimizedMatchingEngine> opt_engine(
        m, "OptimizedMatchingEngine", "Zero-allocation intrusive pool matching engine");
    opt_engine.def(
        py::init([](std::size_t initial_capacity) {
            return OptimizedMatchingEngine(optimized::OptimizedOrderBook(initial_capacity));
        }),
        py::arg("initial_capacity") = optimized::kDefaultCapacity);
    bind_engine_methods(opt_engine);

    // Default alias
    m.attr("MatchingEngine") = opt_engine;

    // -----------------------------------------------------------------------
    // Canonical State & Replay
    // -----------------------------------------------------------------------
    py::class_<CanonicalState>(m, "CanonicalState", "Binary state canonical serializer & hasher")
        .def_static("hash_reference", &CanonicalState::compute_hash<reference::ReferenceOrderBook>)
        .def_static("hash_optimized", &CanonicalState::compute_hash<optimized::OptimizedOrderBook>)
        .def_static("to_hex", [](std::uint64_t hash) {
            std::ostringstream oss;
            oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << hash;
            return oss.str();
        });

    py::class_<ReplayResult>(m, "ReplayResult", "Replay execution result and audit")
        .def_readonly("final_hash", &ReplayResult::final_hash)
        .def_readonly("execution_reports", &ReplayResult::execution_reports)
        .def_readonly("all_trades", &ReplayResult::all_trades)
        .def_readonly("invariants_satisfied", &ReplayResult::invariants_satisfied)
        .def_readonly("failure_reason", &ReplayResult::failure_reason);

    py::class_<ReplayEngine>(m, "ReplayEngine", "Deterministic event replay runner")
        .def_static("replay_reference", &ReplayEngine::replay<reference::ReferenceOrderBook>,
                    py::arg("commands"), "Replay event journal on ReferenceMatchingEngine")
        .def_static("replay_optimized", &ReplayEngine::replay<optimized::OptimizedOrderBook>,
                    py::arg("commands"), "Replay event journal on OptimizedMatchingEngine")
        .def_static("verify_determinism",
                    &ReplayEngine::verify_determinism<reference::ReferenceOrderBook,
                                                      optimized::OptimizedOrderBook>,
                    py::arg("commands"),
                    "Verify bit-for-bit equivalence between Reference and Optimized engines");
}
