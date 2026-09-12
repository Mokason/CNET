# VSA fail-closed arena eval (bin, 48 capsules)

Scoring: `score = n_correct - 2 * n_wrong_accept`. Abstain = 0.
In-domain gold = the capsule the query was drawn from. OOD gold = abstain.
Query sources: probes=0 corpus=96 name=0 skipped_no_text=0.
OOD: kept 10/12 hand aliens (dropped 2 as covered-topic), plus 0 sentences from uncovered corpora.

| set | n | src | legacy accept | new accept | correct | wrong | abstain | top-1 self | new score |
|---|---|---|---|---|---|---|---|---|---|
| in-domain | 96 | probes/corpus | 0.969 | 0.531 | 0.281 | 0.250 | 0.469 | 0.375 | -21 |
| ood | 10 | alien+uncovered | 0.600 | 0.000 | 0.000 | 0.000 | 1.000 | - | 0 |

## Covered-topic queries removed from the alien set
| query | overlapping name tokens |
|---|---|
| How do quantum qubits maintain coherent superposition on a Bloch spher | quantum |
| How are deep-sea coral reef ecosystems affected by ocean acidification | thermal |

z-score: in-domain median 4.72, ood max 3.96, required 4.66

## OOD queries
| query | closest | dist | z | legacy | new |
|---|---|---|---|---|---|
| How do I fold and shape a sourdough loaf for an open crumb? | computer_vision_and_shape_analysis | 0.860 | 3.96 | ACCEPT | refuse |
| What stitch pattern gives a Fair Isle sweater its stranded c | tribunal_procedure | 0.910 | 3.16 | refuse | refuse |
| What are the astrological interpretations of planetary trans | damage_mechanics | 0.801 | 3.01 | ACCEPT | refuse |
| How does a haute couture atelier drape silk velvet for an ev | magnetorheological_actuator_design | 0.898 | 3.07 | ACCEPT | refuse |
| How do paleontologists date dinosaur fossils in sedimentary  | pm_director_pacing | 0.932 | 3.59 | refuse | refuse |
| What gives a wheated bourbon its sweetness during barrel agi | pm_perception_sensors | 0.873 | 3.29 | ACCEPT | refuse |
| Which chess openings lead to a closed center and a kingside  | ceramic_fiber_manufacturing | 0.920 | 3.91 | refuse | refuse |
| How do you tune a violin using harmonics and a fifth interva | pm_craft_production | 0.870 | 3.25 | ACCEPT | refuse |
| What is the best way to train a puppy to stop pulling on the | feudal_jurisprudence_and_estate_law | 0.947 | 3.35 | refuse | refuse |
| How does a sommelier detect cork taint in an aged Burgundy? | detwinning_and_variant_reorientation | 0.853 | 2.96 | ACCEPT | refuse |

## In-domain queries lost to the margin gate (9)
| capsule | src | query | z | z_min | dist |
|---|---|---|---|---|---|
| abstract_interpretation | corpus | This approach avoids full re-analysis after modifi | 3.90 | 4.66 | 0.876 |
| active_feedback_control_systems | corpus | Stability is ensured through Lyapunov-based analys | 4.17 | 4.66 | 0.804 |
| architectural_defense_engineering | corpus | Regular post-installation inspections are required | 3.56 | 4.66 | 0.844 |
| asynchronous_network_models | corpus | Consistency models must be carefully selected, as  | 4.45 | 4.66 | 0.823 |
| atom_probe_tomography | corpus | The reconstruction process involves iterative refi | 4.09 | 4.66 | 0.835 |
| automated_fixture_clamping_mechanisms | corpus | Digital twin simulations optimize clamp positionin | 4.62 | 4.66 | 0.823 |
| b_tree_indexing | corpus | The optimal order is achieved by maximizing the br | 4.41 | 4.66 | 0.815 |
| beam_splitter_calibration | corpus | Polarization-based techniques require precise pola | 4.09 | 4.66 | 0.752 |
| biological_water_treatment | corpus | Ammonia toxicity occurs when total ammonia nitroge | 2.97 | 4.66 | 0.892 |

WITHHELD: generated-text quality; transformer baseline (see cnet_vsa_vs_transformer.py).
