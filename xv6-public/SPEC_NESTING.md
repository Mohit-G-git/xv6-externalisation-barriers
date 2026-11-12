# Nested speculative scopes

Each `xbegin()` creates a context with a parent context ID. A process keeps a
per-scope stack of its enclosing context and saved current directory. Forked
processes inherit the complete stack and join the current context.

Nested filesystem reads search the current shadow map and then each ancestor
map. Resource reservations follow the same ancestry rule, so a nested scope can
use an inode or block reserved by its parent without exposing it to unrelated
contexts. The child keeps its own writes, read set, reservations, inode-cache
view, file handles and buffered output until it terminates.

An inner `xabort()` discards only the current child context and restores its
participants to the parent context. An inner `xcommit()` freezes the child and
parent, drains their outstanding operations, then merges child block images,
read-set entries, resource reservations and buffered output into the parent.
An existing parent shadow image is overwritten by the child image while
retaining the parent's original base version. Parent inode views are invalidated
after the merge so their metadata is reloaded from the merged shadow map.

The parent is unavailable for new operations while a nested merge is in
progress. A context supports one active nested child at a time, and a parent
cannot commit or abort while that child remains active. This gives the current
implementation strict stack ordering. Only an outermost successful commit
publishes blocks through the journal and publishes buffered output. Aborting an
outer scope discards every nested change already merged into it. Forked
participants in an aborted outer scope remain attached to the aborted context,
so their system calls stay blocked until they exit or call `xbegin()` to detach
and start a fresh scope.

The nested test program covers multiple nesting levels, merge followed by
selective abort, outermost publication, and a forked participant in an inner
scope. Nested pipe reads do not consume bytes buffered by an ancestor context;
pipe data written by a nested context is retagged to its parent on merge.
