import unittest
from lobsim.engine import PaperTradingSimulator
from lobsim.lob_event import NormalizedLobEvent
from lobsim.types import Side, UpdateType
from lobsim._core import MetricsSink


class TestMetricsSink(unittest.TestCase):
    def test_metrics_calculation(self):
        # Setup engine and sink
        engine = PaperTradingSimulator()
        sink = MetricsSink(engine)
        engine.set_log_sink(sink)

        # 1. Add Bid @ 100, Qty 10
        engine.update(
            NormalizedLobEvent(
                price_ticks=100,
                quantity_lots=10,
                side=Side.BUY,
                update_type=UpdateType.ADD,
                order_id=1,
            )
        )

        # Metrics should be updated (but no spread yet as no ask)
        metrics = sink.get_metrics()
        self.assertEqual(len(metrics), 1)
        self.assertEqual(metrics[0].best_bid, 100)
        self.assertIsNone(metrics[0].best_ask)
        self.assertIsNone(metrics[0].spread)

        # 2. Add Ask @ 110, Qty 10
        engine.update(
            NormalizedLobEvent(
                price_ticks=110,
                quantity_lots=10,
                side=Side.SELL,
                update_type=UpdateType.ADD,
                order_id=2,
            )
        )

        metrics = sink.get_metrics()
        self.assertEqual(len(metrics), 2)
        last_metric = metrics[-1]

        self.assertEqual(last_metric.best_bid, 100)
        self.assertEqual(last_metric.best_ask, 110)
        self.assertEqual(last_metric.spread, 10)  # 110 - 100
        self.assertEqual(last_metric.mid_price, 105.0)  # (110+100)/2
        self.assertAlmostEqual(last_metric.imbalance, 0.0)  # (10-10)/20 = 0

        # 3. Add more Bid @ 100, Qty 20 -> Total Bid 30
        engine.update(
            NormalizedLobEvent(
                price_ticks=100,
                quantity_lots=20,
                side=Side.BUY,
                update_type=UpdateType.ADD,
                order_id=3,
            )
        )

        metrics = sink.get_metrics()
        last_metric = metrics[-1]

        # Imbalance = (30 - 10) / (30 + 10) = 20 / 40 = 0.5
        self.assertAlmostEqual(last_metric.imbalance, 0.5)


if __name__ == "__main__":
    unittest.main()
