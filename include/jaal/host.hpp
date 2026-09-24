#pragma once
// jaal::host — the hosts that don't draw: for tests and simulation.
//
//   #include <jaal/host.hpp>
//
//   given<P>     update tested as data, no kernel and no clock
//   headless<P>  a real kernel on a fake clock; records effects
//   sim<P>       a whole run from one seed; explore() hunts for a bad one
//
// A host that DOES draw (a terminal, a window) is written against the host
// protocol in <jaal/kernel/run.hpp>; docs/hosts.md is the guide.
//
// Layer umbrellas (docs/design.md §7): <jaal/meta.hpp>, <jaal/core.hpp>,
// <jaal/kernel.hpp>, <jaal/platform.hpp>, <jaal/host.hpp>, and
// <jaal/jaal.hpp> for everything.

#include "host/given.hpp"
#include "host/headless.hpp"
#include "host/sim.hpp"
