/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1HeapEvaluationTask.hpp"
#include "gc/g1/g1HeapSizingPolicy.hpp"
#include "gc/g1/g1ServiceThread.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "logging/log.hpp"

G1HeapEvaluationTask::G1HeapEvaluationTask(G1CollectedHeap* g1h, G1HeapSizingPolicy* heap_sizing_policy) :
  G1ServiceTask("G1 Heap Evaluation Task"),
  _g1h(g1h),
  _heap_sizing_policy(heap_sizing_policy),
  _idle_evaluation_count(0) {
}

void G1HeapEvaluationTask::execute() {
  log_debug(gc, ergo, heap)("Starting uncommit evaluation");

  bool should_attempt;

  // Join STS for GC synchronization during the lightweight pre-check.
  // Do NOT acquire Heap_lock here: holding STS while blocking on Heap_lock
  // deadlocks because a safepoint calls STS::synchronize().
  {
    SuspendibleThreadSetJoiner sts;
    should_attempt = _heap_sizing_policy->should_attempt_uncommit();
  }

  if (should_attempt) {
    _idle_evaluation_count = 0;
    _g1h->request_heap_shrink();
  } else {
    _idle_evaluation_count++;
    if (_idle_evaluation_count % 10 == 0) {
      log_debug(gc, ergo, heap)("Uncommit evaluation: no action for %d consecutive checks",
                                _idle_evaluation_count);
    }
  }

  schedule(G1TimeBasedEvaluationIntervalMillis);
}
