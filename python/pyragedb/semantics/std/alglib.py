import logging
import warnings

import requests

from ..relationship import Relationship

logger = logging.getLogger(__name__)

REQUEST_TIMEOUT_SECONDS = 10


def _register_property(rel: Relationship, prop: str, model=None):
    """Record an algebraic trait on the relationship and synchronize it to the RageDB catalog.

    The model owning the relationship must be passed explicitly (as the Model.alglib
    property does); only the module-level helpers fall back to the active model.
    Failures to reach the server are logged as warnings: the trait stays recorded
    client-side and can be re-synced, but queries will run unoptimized until then.
    """
    if not isinstance(rel, Relationship):
        raise TypeError("alglib relation properties can only be declared on Relationship types.")

    if model is None:
        from .. import get_active_model
        model = get_active_model()

    if not hasattr(rel, "_algebraic_properties"):
        rel._algebraic_properties = set()
    rel._algebraic_properties.add(prop)

    # Relationship types are registered upper-cased by Model (see Model.Relationship), so the
    # algebra endpoint must be addressed the same way.
    rel_type = rel.name.upper()
    try:
        response = requests.post(
            f"{model.host}/db/{model.graph}/schema/relationships/{rel_type}/algebra",
            json=sorted(rel._algebraic_properties),
            timeout=REQUEST_TIMEOUT_SECONDS,
        )
        response.raise_for_status()
    except requests.RequestException as exc:
        logger.warning(
            "Failed to sync algebraic trait %r for relationship %s to %s: %s",
            prop, rel_type, model.host, exc,
        )


def _warn_domain_unsupported(fn_name: str, domain):
    if domain is not None:
        warnings.warn(
            f"{fn_name}(..., domain={domain!r}): the domain argument is not yet enforced by the "
            "server; reflexivity is currently treated as global. Pass domain=None to silence.",
            UserWarning,
            stacklevel=3,
        )


def symmetric(rel: Relationship, model=None):
    _register_property(rel, "symmetric", model=model)


def transitive(rel: Relationship, model=None):
    _register_property(rel, "transitive", model=model)


def reflexive(rel: Relationship, domain, model=None):
    _warn_domain_unsupported("reflexive", domain)
    _register_property(rel, "reflexive", model=model)


def irreflexive(rel: Relationship, model=None):
    _register_property(rel, "irreflexive", model=model)


def antisymmetric(rel: Relationship, model=None):
    _register_property(rel, "antisymmetric", model=model)


def equivalence_relation(rel: Relationship, domain, model=None):
    _warn_domain_unsupported("equivalence_relation", domain)
    reflexive(rel, None, model=model)
    symmetric(rel, model=model)
    transitive(rel, model=model)


class ModelAlglib:
    """Model-bound view of the alglib helpers: traits declared through `model.alglib` always sync
    to that model's host/graph, regardless of which model was constructed last."""

    def __init__(self, model):
        self._model = model

    def symmetric(self, rel):
        symmetric(rel, model=self._model)

    def transitive(self, rel):
        transitive(rel, model=self._model)

    def reflexive(self, rel, domain):
        reflexive(rel, domain, model=self._model)

    def irreflexive(self, rel):
        irreflexive(rel, model=self._model)

    def antisymmetric(self, rel):
        antisymmetric(rel, model=self._model)

    def equivalence_relation(self, rel, domain):
        equivalence_relation(rel, domain, model=self._model)
