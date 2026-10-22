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
 */

package gc.g1;

/**
 * @test TestG1RegionUncommit
 * @bug 8357445
 * @summary Regression for the time-based uncommit free-list/safepoint bug
 *          (microsoft/openjdk#677): start with a large committed but idle heap and let the
 *          time-based evaluation uncommit the idle free regions, asserting it happened
 *          ("Time-based shrink: deactivated") so the free-list-mutation-at-safepoint path
 *          is actually exercised. On fastdebug the master free-list MT-safety guarantee is
 *          active, so an off-safepoint regression aborts the VM.
 * @requires vm.gc.G1
 * @library /test/lib
 * @run main gc.g1.TestG1RegionUncommit
 */

import java.util.concurrent.atomic.AtomicBoolean;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestG1RegionUncommit {

    // Sink for the background mutator's allocations so they are not optimized away.
    static volatile byte[] blackhole;

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            stress();
            return;
        }

        OutputAnalyzer o = new OutputAnalyzer(ProcessTools.createLimitedTestJavaProcessBuilder(
            "-XX:+UseG1GC",
            "-XX:+UnlockDiagnosticVMOptions",
            // Start with a large committed heap (InitialHeapSize) but a low floor
            // (MinHeapSize) so there is a big pool of never-touched free regions to uncommit.
            "-XX:InitialHeapSize=200m", "-XX:MinHeapSize=16m", "-Xmx256m",
            "-XX:G1HeapRegionSize=1M",
            // Disable GC-based shrink so any uncommit is attributable to the time-based path.
            "-XX:MaxHeapFreeRatio=100",
            "-XX:G1TimeBasedEvaluationIntervalMillis=1000",
            "-XX:G1UncommitDelayMillis=1000",
            "-XX:G1MinRegionsToUncommit=1",
            "-Xlog:gc+ergo+heap=debug",
            "gc.g1.TestG1RegionUncommit", "stress").start());

        // The periodic evaluation must have run...
        o.shouldContain("Starting uncommit evaluation");
        // ...and once the app went idle it must have actually uncommitted at least one
        // region, i.e. the free-list-mutation-at-safepoint path (the #677 crash site)
        // really executed. Without this a pass would only prove the periodic task ran.
        o.shouldContain("Time-based shrink: deactivated");
        // The master free-list MT-safety guarantee has teeth on fastdebug: a trip aborts
        // the VM (non-zero exit).
        o.shouldHaveExitValue(0);
    }

    static void stress() throws Exception {
        // The heap starts committed at ~200m (InitialHeapSize) with a 16m floor
        // (MinHeapSize), so most regions are free and never touched. With no bulk
        // allocation there are no GCs, so GC overhead stays under the uncommit pre-check
        // threshold, and the untouched free regions read as idle. The time-based
        // evaluation therefore uncommits them down toward the floor at a safepoint over
        // several cycles. A light daemon thread stays alive so the safepoint uncommit runs
        // with a live mutator present (teeth for the fastdebug master free-list guarantee).
        AtomicBoolean stop = new AtomicBoolean(false);
        Thread mutator = new Thread(() -> {
            while (!stop.get()) {
                blackhole = new byte[4 * 1024];
                try {
                    Thread.sleep(200);
                } catch (InterruptedException e) {
                    return;
                }
            }
        });
        mutator.setDaemon(true);
        mutator.start();

        // Well beyond G1UncommitDelayMillis + several evaluation intervals.
        Thread.sleep(10000);

        stop.set(true);
        mutator.join(2000);
    }
}
