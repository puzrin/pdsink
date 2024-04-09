@initialize:python@ @@
import os

recipe_dir = os.path.dirname(cocci.cocci_file)

with open(os.path.join(recipe_dir, 'pe_state_to_remove.txt'), 'r') as f:
    fsm_states = [line.strip() for line in f if line.strip() and not line.startswith('#')]

@r1@
type T;
identifier I, N;
expression E;
position p : script:python(N) { N in fsm_states };
@@
(
T I[] = {
    ...,
-   [N@p] = E,
    ...
};
|
T I[] = {
    ...,
-   [N@p] = {...},
    ...
};
)