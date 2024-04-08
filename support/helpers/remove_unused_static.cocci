@r@ type T; identifier I; @@
static T I(...) { ... }

@useful@ identifier r.I; @@
I

@remove depends on !useful@ type T; identifier r.I; @@
- static T I(...) { ... }
