import unittest
import warnings
from unittest.mock import patch

import requests

from pyragedb.semantics import Model
from pyragedb.semantics.std import alglib


class TestAlglib(unittest.TestCase):
    """All server traffic is mocked: these tests assert the exact URL, payload, and type casing
    posted to the algebra endpoint without touching a live RageDB."""

    def _model(self, name="alglib_test_model", host="http://db-one:7243", graph="g1"):
        return Model(name, host=host, graph=graph)

    @staticmethod
    def _algebra_calls(mock_post):
        """Model.Concept/Relationship also POST schema registrations through the same module;
        only the algebra-endpoint calls are under test here."""
        return [c for c in mock_post.call_args_list if c.args[0].endswith("/algebra")]

    def test_declarations_record_and_post_traits(self):
        with patch("pyragedb.semantics.std.alglib.requests.post") as mock_post:
            m = self._model()
            person = m.Concept("Person")
            knows = m.Relationship(f"{person} knows {person:friend}")

            alglib.symmetric(knows)
            self.assertIn("symmetric", knows._algebraic_properties)

            calls = self._algebra_calls(mock_post)
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0].args[0],
                "http://db-one:7243/db/g1/schema/relationships/" + knows.name.upper() + "/algebra")
            self.assertEqual(calls[0].kwargs["json"], ["symmetric"])
            self.assertIn("timeout", calls[0].kwargs)

    def test_payload_is_cumulative_and_sorted(self):
        with patch("pyragedb.semantics.std.alglib.requests.post") as mock_post:
            m = self._model()
            person = m.Concept("Person")
            ancestor_of = m.Relationship(f"{person} ancestor_of {person:descendant}")

            alglib.transitive(ancestor_of)
            alglib.irreflexive(ancestor_of)

            calls = self._algebra_calls(mock_post)
            self.assertEqual(len(calls), 2)
            self.assertEqual(calls[0].kwargs["json"], ["transitive"])
            self.assertEqual(calls[1].kwargs["json"], ["irreflexive", "transitive"])

    def test_model_alglib_binds_to_owning_model(self):
        with patch("pyragedb.semantics.std.alglib.requests.post") as mock_post:
            m1 = self._model("m1", host="http://db-one:7243", graph="g1")
            person = m1.Concept("Person")
            precedes = m1.Relationship(f"{person} precedes {person:other}")
            m1_alglib = m1.alglib

            # Constructing a second model makes it the "active" one; m1.alglib must still post to m1.
            self._model("m2", host="http://db-two:7243", graph="g2")

            m1_alglib.antisymmetric(precedes)
            self.assertIn("antisymmetric", precedes._algebraic_properties)
            url = self._algebra_calls(mock_post)[-1].args[0]
            self.assertTrue(url.startswith("http://db-one:7243/db/g1/"), url)

    def test_equivalence_relation_sets_all_three_traits(self):
        with patch("pyragedb.semantics.std.alglib.requests.post") as mock_post:
            m = self._model()
            person = m.Concept("Person")
            same_clan = m.Relationship(f"{person} same_clan {person:relative}")

            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("always")
                m.alglib.equivalence_relation(same_clan, domain=person)

            for trait in ("reflexive", "symmetric", "transitive"):
                self.assertIn(trait, same_clan._algebraic_properties)
            # The final POST carries the cumulative trait set.
            self.assertEqual(self._algebra_calls(mock_post)[-1].kwargs["json"],
                             ["reflexive", "symmetric", "transitive"])
            # The unsupported domain argument warns instead of being silently dropped.
            self.assertTrue(any("domain" in str(w.message) for w in caught))

    def test_non_relationship_raises_type_error(self):
        with patch("pyragedb.semantics.std.alglib.requests.post") as mock_post:
            m = self._model()
            person = m.Concept("Person")
            with self.assertRaises(TypeError):
                alglib.symmetric(person)
            self.assertEqual(self._algebra_calls(mock_post), [])

    def test_sync_failure_warns_but_keeps_client_state(self):
        m = self._model()
        person = m.Concept("Person")
        knows = m.Relationship(f"{person} knows {person:friend}")
        with patch("pyragedb.semantics.std.alglib.requests.post",
                   side_effect=requests.ConnectionError("refused")) as mock_post:
            with self.assertLogs("pyragedb.semantics.std.alglib", level="WARNING") as logs:
                alglib.symmetric(knows)
            self.assertIn("symmetric", knows._algebraic_properties)
            self.assertTrue(any("Failed to sync" in line for line in logs.output))
            mock_post.assert_called_once()


if __name__ == "__main__":
    unittest.main()
