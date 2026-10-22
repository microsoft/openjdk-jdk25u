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
 * @test TestTimeBasedHeapConfig
 * @bug 8357445
 * @summary Validate flag parsing and constraints for time-based G1 heap uncommit.
 * @requires vm.gc.G1
 * @library /test/lib
 * @run main gc.g1.TestTimeBasedHeapConfig
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestTimeBasedHeapConfig {

    public static void main(String[] args) throws Exception {
        // Interval must be a multiple of PeriodicTask::interval_gran (10).
        expectRejected(
            new String[] {"-XX:+UseG1GC", "-XX:G1TimeBasedEvaluationIntervalMillis=1005", "-version"},
            "must be evenly divisible by PeriodicTask::interval_gran");

        // Interval below the allowed minimum (1000).
        expectRejected(
            new String[] {"-XX:+UseG1GC", "-XX:G1TimeBasedEvaluationIntervalMillis=500", "-version"},
            "G1TimeBasedEvaluationIntervalMillis");

        // Uncommit delay below the allowed minimum (1000).
        expectRejected(
            new String[] {"-XX:+UseG1GC", "-XX:G1UncommitDelayMillis=500", "-version"},
            "G1UncommitDelayMillis");

        // Minimum-regions below the allowed minimum (1); diagnostic flag.
        expectRejected(
            new String[] {"-XX:+UseG1GC", "-XX:+UnlockDiagnosticVMOptions",
                          "-XX:G1MinRegionsToUncommit=0", "-version"},
            "G1MinRegionsToUncommit");

        // A valid configuration must start normally.
        OutputAnalyzer ok = run(new String[] {
            "-XX:+UseG1GC", "-XX:+UnlockDiagnosticVMOptions",
            "-XX:G1TimeBasedEvaluationIntervalMillis=2000",
            "-XX:G1UncommitDelayMillis=2000",
            "-XX:G1MinRegionsToUncommit=1",
            "-version"});
        ok.shouldHaveExitValue(0);
    }

    private static OutputAnalyzer run(String[] opts) throws Exception {
        return new OutputAnalyzer(ProcessTools.createLimitedTestJavaProcessBuilder(opts).start());
    }

    private static void expectRejected(String[] opts, String expectedMessage) throws Exception {
        OutputAnalyzer o = run(opts);
        o.shouldContain(expectedMessage);
        o.shouldNotHaveExitValue(0);
    }
}
