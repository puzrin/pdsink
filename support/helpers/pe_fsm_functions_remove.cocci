@initialize:python@ @@
import os

recipe_dir = os.path.dirname(cocci.cocci_file)

with open(os.path.join(recipe_dir, 'pe_state_to_remove.txt'), 'r') as f:
    fsm_states = [line.strip() for line in f if line.strip() and not line.startswith('#')]

fsm_handlers = []

for s in fsm_states:
    s = s.lower()
    fsm_handlers.extend([f'{s}_entry', f'{s}_run', f'{s}_exit'])

@r@ identifier I; @@
I(...) { ... }

@script:python@ I << r.I; @@
if I not in fsm_handlers: cocci.include_match(False)

@remove@ identifier r.I; @@
- I(...) { ... }
