import unittest

from lobsim import BookId
from lobsim.engine import PaperTradingSimulator
from lobsim.lob_event import NormalizedLobEvent
from lobsim.multibook import Config, MultiBookSimulator
from lobsim.sink import InMemoryLogSink, InMemoryMultiLogSink
from lobsim.types import Side, UpdateSource, UpdateType


class TestPublicAPI(unittest.TestCase):
    def test_core_symbols(self) -> None:
        self.assertTrue(hasattr(UpdateType, "AGGRESSIVE_TRADE"))
        self.assertIsNotNone(UpdateSource.STRATEGY)
        self.assertIsNotNone(Side.BUY)

    def test_engine_surface(self) -> None:
        engine = PaperTradingSimulator()
        sink = InMemoryLogSink()
        engine.set_log_sink(sink)

        engine.init_from_l2_snapshot([], [], [])
        engine.init_from_l3_snapshot([], [], [], [], [])

        ev = NormalizedLobEvent(
            ts_exchange=1,
            ts_received=1,
            side=Side.BUY,
            update_type=UpdateType.ADD,
            price_ticks=100,
            quantity_lots=10,
            order_id=1,
            update_source=UpdateSource.HISTORICAL,
            symbol_id="venue:symbol",
        )
        engine.update(ev)

        self.assertEqual(engine.depth_at(Side.BUY, 100), 10)
        self.assertEqual(engine.get_best_price_ticks(Side.BUY), 100)
        self.assertEqual(engine.l2_top_n(Side.BUY, 1), [(100, 10)])

    def test_multibook_surface(self) -> None:
        sim = MultiBookSimulator(Config())
        sink = InMemoryMultiLogSink()
        sim.set_multi_log_sink(sink)

        book_id = BookId("venue", "symbol")
        self.assertTrue(sim.add_book(book_id))
        self.assertTrue(sim.has_book(book_id))

        ev_hist = NormalizedLobEvent(
            ts_exchange=1,
            ts_received=1,
            side=Side.BUY,
            update_type=UpdateType.ADD,
            price_ticks=100,
            quantity_lots=10,
            order_id=111,
            update_source=UpdateSource.HISTORICAL,
            symbol_id=book_id.book_key,
        )
        sim.apply(book_id, ev_hist)

        self.assertEqual(sim.get_best_price_ticks(book_id, Side.BUY), 100)
        self.assertEqual(sim.depth_at(book_id, Side.BUY, 100), 10)
        self.assertEqual(sim.l2_top_n(book_id, Side.BUY, 1), [(100, 10)])

        ev_strat = NormalizedLobEvent(
            ts_exchange=2,
            ts_received=2,
            side=Side.BUY,
            update_type=UpdateType.ADD,
            price_ticks=100,
            quantity_lots=5,
            order_id=222,
            update_source=UpdateSource.STRATEGY,
            symbol_id=book_id.book_key,
        )
        sim.submit_strategy_event(book_id, ev_strat)
        self.assertTrue(sim.step())


if __name__ == "__main__":
    unittest.main()
