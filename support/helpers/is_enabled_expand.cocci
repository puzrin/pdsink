// NOT WORKS! Use python script instead.

@initialize:python@ @@
import os
import re

recipe_dir = os.path.dirname(cocci.cocci_file)

with open(os.path.join(recipe_dir, '../../src/pd_config.h'), 'r') as f:
    h_file = f.read()
defined_variables = re.findall(r'#define (\w+)', h_file)
undefined_variables = re.findall(r'#undef (\w+)', h_file)

@r_on exists@
identifier VAR;
position p : script:python(VAR) { VAR in defined_variables || VAR in undefined_variables};
@@
  IS_ENABLED(VAR)@p

@script:python pr@
VAR << r_on.VAR;
K;
@@
coccinelle.K = cocci.make_ident("/*foo*/")

@replace depends on pr@
identifier r_on.VAR;
position r_on.p;
comment K = pr.K;
@@
 IS_ENABLED(VAR)@p
++K