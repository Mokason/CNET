"""Small exhaustive references first; no native/GPU work in these unit tests."""
import itertools
import random
import time
import unittest

from allocator_sequence_pilot import coverage, reachable_requirements, plan, make_episode


def edge(source, target, keys=range(64), operand=0, age=0, bad=False):
    return {"input": source, "output": target, "keys": list(keys),
            "operand": operand, "cost": len(keys), "age": age, "rejected": bad}


class SequencePilotTests(unittest.TestCase):
    def test_pair_is_not_singleton_union(self):
        tasks = [edge("a", "b"), edge("b", "c")]
        rows = reachable_requirements(tasks, [("a", "c", 4)])
        self.assertEqual(rows, [(3,)])
        self.assertEqual([coverage(rows, i) for i in range(4)], [0, 0, 0, 1])

    def test_coverage_checks_transformed_value_at_every_hop(self):
        tasks = [edge("a", "b", [0, 1], 1), edge("b", "c", [1])]
        self.assertEqual(reachable_requirements(tasks, [("a", "c", 0), ("a", "c", 1)]), [(3,), ()])

    def test_alternative_paths_and_installed_prefix(self):
        tasks = [edge("b", "c"), edge("a", "c")]
        rows = reachable_requirements(tasks, [("a", "c", 2)], [edge("a", "b")])
        self.assertEqual(rows, [(1, 2)])
        self.assertEqual(coverage(rows, 3), 1)

    def test_shared_prefix_charged_once_and_bundle_completes(self):
        tasks = [edge("a", "b"), edge("b", "c"), edge("b", "d"), edge("a", "e")]
        rows = [(3,), (5,), (8,)]
        result = plan(tasks, rows, "bundle", jobs=3, work=192)
        self.assertEqual(result["value"], 2)
        self.assertEqual(len(result["actions"]), len(set(result["actions"])))
        self.assertEqual(result["work"], 192)

    def test_real_cursor_and_overdue_prefix_have_equal_budgets(self):
        tasks = [edge("a", "b", range(3), age=4), edge("a", "c", range(2)), edge("a", "d", range(2))]
        result = plan(tasks, [(1,), (2,), (4,)], "cursor", jobs=2, work=5, cursor=2)
        self.assertEqual(result["actions"], [0, 2])
        self.assertEqual(result["work"], 5)

    def test_rejected_forced_evidence_charged_but_never_covered(self):
        tasks = [edge("a", "b", range(2), age=4, bad=True), edge("a", "c", range(2))]
        result = plan(tasks, [(1,), (2,)], "exact", jobs=2, work=4)
        self.assertEqual(result["actions"], [0, 1])
        self.assertEqual(result["work"], 4)
        self.assertEqual(result["value"], 1)
        self.assertTrue(result["complete"])

    def test_exact_matches_independent_exhaustive_all_subsets(self):
        tasks = [edge("a", "b", range(c)) for c in [3, 4, 2, 5, 1, 3]]
        rows = [(3, 20), (12,), (48,), (1, 32), (6,), (16,)]
        expected = 0
        for size in range(4):
            for subset in itertools.combinations(range(6), size):
                if sum(tasks[i]["cost"] for i in subset) <= 8:
                    mask = sum(1 << i for i in subset)
                    expected = max(expected, sum(any(mask & path == path for path in paths) for paths in rows))
        result = plan(tasks, rows, "exact", jobs=3, work=8)
        self.assertEqual(result["value"], expected)
        self.assertEqual(result["upper_bound"], expected)
        self.assertTrue(result["complete"])

    def test_timeout_is_not_optimality_claim(self):
        ticks = iter([0, 2, 3, 4, 5, 6, 7, 8, 9])
        result = plan([edge("a", "b", range(2)) for _ in range(6)],
                      [(3,), (5,), (6,)], "exact", jobs=2, work=4,
                      cpu_seconds=1, clock=lambda: next(ticks))
        self.assertFalse(result["complete"])
        self.assertGreaterEqual(result["upper_bound"], result["value"])
        self.assertLessEqual(len(result["actions"]), 2)

    def test_invalid_caps_refused_before_search(self):
        for jobs, work, seconds in [(0, 1, 1), (9, 1, 1), (1, 0, 1), (1, 513, 1), (1, 1, 0), (1, 1, 31)]:
            with self.subTest(jobs=jobs, work=work, seconds=seconds):
                with self.assertRaises(ValueError):
                    plan([edge("a", "b")], [(1,)], "exact", jobs=jobs, work=work, cpu_seconds=seconds)

    def test_randomized_bound_and_optimum_against_exhaustive(self):
        rng = random.Random(69060701)
        for episode in range(100):
            tasks = [edge("a", "b", range(rng.randint(1, 8))) for _ in range(8)]
            rows = [tuple(sorted({rng.randrange(1, 256) for _ in range(rng.randint(1, 3))})) for _ in range(12)]
            budget, jobs = rng.randint(8, 24), rng.randint(1, 5)
            expected = max(coverage(rows, mask) for mask in range(256)
                           if mask.bit_count() <= jobs and sum(task["cost"] for i, task in enumerate(tasks) if mask >> i & 1) <= budget)
            result = plan(tasks, rows, "exact", jobs=jobs, work=budget)
            with self.subTest(episode=episode):
                self.assertTrue(result["complete"])
                self.assertEqual(result["value"], expected)
                self.assertEqual(result["upper_bound"], expected)

    def test_completion_preserves_native_demand_tie_break(self):
        tasks = [edge("a", "b", range(2)), edge("a", "c", range(2))]
        tasks[0]["features"], tasks[1]["features"] = [0.25, 0, 0], [0.5, 0, 0]
        self.assertEqual(plan(tasks, [(1,), (2,)], "completion", jobs=1, work=2)["actions"], [1])

    def test_initialization_overrun_is_explicit(self):
        ticks = iter([0, 2, 3, 4])
        result = plan([edge("a", "b") for _ in range(3)], [(3,), (5,), (6,)],
                      "exact", jobs=2, work=128, clock=lambda: next(ticks))
        self.assertTrue(result["initialization_overrun"])
        self.assertGreater(result["cap_overrun_seconds"], 0)
        self.assertFalse(result["complete"])

    def test_frozen_generator_bounded_and_all_fairness_prefixes_fit(self):
        for number in range(16):
            episode = make_episode(number)
            self.assertEqual(len(episode["tasks"]), 32)
            self.assertEqual(len(episode["requests"]), 64)
            self.assertEqual(len(set(map(tuple, episode["requests"]))), 64)
            self.assertEqual([i for i, task in enumerate(episode["tasks"]) if task["age"] == 4], [0])
            self.assertTrue(all(48 <= task["cost"] <= 64 and task["features"][0] > 0 for task in episode["tasks"]))


if __name__ == "__main__":
    unittest.main()
