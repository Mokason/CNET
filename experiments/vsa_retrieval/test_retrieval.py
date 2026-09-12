"""Run with python3 -m unittest discover -s experiments/vsa_retrieval -v."""
import tempfile
import unittest
from pathlib import Path

import numpy as np

from retrieval import Native, SparseIndex, load_corpora, prototypes, reciprocal_rank_fusion


class RetrievalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native = Native()

    def test_test_rows_never_enter_index(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "corpora.tsv"
            path.write_text("a\ttrain\tallowed\na\ttest\tSECRET\nb\ttrain\tother\n")
            names, train, owners, tests, gold = load_corpora(path)
            self.assertEqual(names, ["a", "b"])
            self.assertEqual(train, ["allowed", "other"])
            self.assertEqual(owners.tolist(), [0, 1])
            self.assertEqual(tests, ["SECRET"])
            self.assertEqual(gold.tolist(), [0])

    def test_unknown_role_fails_loud(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "corpora.tsv"
            path.write_text("a\ttrian\tforbidden\n")
            with self.assertRaises(ValueError):
                load_corpora(path)

    def test_prototypes_keep_disjoint_topics(self):
        docs = np.zeros((4, self.native.dim), np.float32)
        docs[0, 0] = docs[1, 0] = 1
        docs[2, 1] = docs[3, 1] = 1
        owner = np.zeros(4, np.int32)
        p, ids = prototypes(docs, owner, 1, 2, self.native)
        q = self.native.quantize(docs[[0, 2]])
        scores = self.native.dense(q, p, ids, 1)
        np.testing.assert_allclose(scores, 1, atol=1e-6)
        p2, ids2 = prototypes(docs, owner, 1, 2, self.native)
        np.testing.assert_array_equal(p, p2)
        np.testing.assert_array_equal(ids, ids2)

    def test_duplicate_prototypes_do_not_duplicate_capsules(self):
        docs = np.zeros((3, self.native.dim), np.float32)
        docs[:2, 0] = 1
        docs[2, 1] = 1
        vectors = self.native.quantize(docs)
        scores = self.native.dense(vectors[:1], vectors, np.array([0, 0, 1], np.int32), 2)
        self.assertEqual(scores.shape, (1, 2))
        np.testing.assert_allclose(scores[0], [1, 0], atol=1e-6)

    def test_sparse_recovers_rare_evidence_and_empty_abstains(self):
        index = SparseIndex([[11, 12, 12], [11, 13]], [0, 1], 2)
        scores = index.score([12], self.native)
        self.assertGreater(scores[0], scores[1])
        np.testing.assert_array_equal(index.score([999], self.native), [0, 0])
        np.testing.assert_array_equal(index.score([], self.native), [0, 0])

    def test_fusion_does_not_invent_evidence_for_zero_sparse_scores(self):
        dense = np.array([[0.2, 0.8, 0.1]], np.float32)
        sparse = np.zeros((1, 3), np.float32)
        fused = reciprocal_rank_fusion(dense, sparse, k=2)
        self.assertEqual(int(fused.argmax()), 1)
        self.assertEqual(float(fused[0, 2]), 0)

    def test_passage_matches_must_cooccur(self):
        from retrieval import PassageIndex
        index = PassageIndex([[11], [12], [11, 12], [12, 11]], [0, 0, 1, 2], 3)
        scores = index.score([11, 12], [0, 1, 2], self.native)
        self.assertGreater(scores[1], scores[0])
        self.assertGreater(scores[1], scores[2])
        np.testing.assert_array_equal(index.score([], [0, 1], self.native), [0, 0, 0])

    def test_bad_native_owner_is_refused_before_access(self):
        docs = np.zeros((1, self.native.dim), np.int8)
        with self.assertRaises(ValueError):
            self.native.dense(docs, docs, np.array([1], np.int32), 1)

    def test_compiled_weights_match_reference_dot_product(self):
        matrix = np.array([[2, 1, 0], [0, 3, 4]], np.float32)
        idf = np.array([0.5, 2, 1], np.float32)
        index = SparseIndex.from_weights(matrix, idf, 3)
        np.testing.assert_allclose(index.score([0, 1, 1], self.native), [3, 6])
        with self.assertRaises(ValueError):
            SparseIndex.from_weights(matrix * np.nan, idf, 3)

    def test_semantic_chunks_do_not_mix_unrelated_evidence(self):
        from retrieval import SemanticChunks
        terms = np.array([[0, 1], [0, 1], [0, 1]], np.uint16)
        weights = np.array([[1, 0], [0, 1], [0.75, 0.75]], np.float32)
        index = SemanticChunks(terms, weights, np.array([0, 0, 1]), np.ones(2), 2)
        np.testing.assert_allclose(index.score([0, 1], self.native), [1, 1.5])

    def test_bad_serialized_postings_are_refused(self):
        from runtime import unpack_sparse
        arrays = {"xkeys": np.array([3], np.uint64), "xoffset": np.array([0, 1], np.uint32),
                  "xowners": np.array([0], np.int32), "xweights": np.array([2], np.float32)}
        index = unpack_sparse(arrays, "x", 1)
        np.testing.assert_array_equal(index.score([3], self.native), [2])
        arrays["xowners"] = np.array([1], np.int32)
        with self.assertRaises(ValueError):
            unpack_sparse(arrays, "x", 1)

    def test_runtime_rejects_bad_query_before_native_call(self):
        from runtime import Retriever
        retriever = Retriever.__new__(Retriever)
        for text in ("hidden\0suffix", "x" * 4097, 3):
            with self.assertRaises(ValueError):
                retriever.scores(text)


if __name__ == "__main__":
    unittest.main()
