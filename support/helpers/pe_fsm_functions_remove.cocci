@initialize:python@
@@
import os

with open('support/helpers/pe_state_to_remove.txt', 'r') as f:
    fsm_states = [line.strip() for line in f if line.strip() and not line.startswith('#')]

fsm_handlers = []

for s in fsm_states:
    s = s.lower()
    fsm_handlers.append(s + '_entry')
    fsm_handlers.append(s + '_run')
    fsm_handlers.append(s + '_exit')

# Return the function names as a Python list
fsm_handlers

@r@
identifier I;
@@
I(...) { ... }

@script:python@
I << r.I;
@@
if I not in fsm_handlers: cocci.include_match(False)

@remove@
identifier r.I;
@@
- I(...) { ... }
