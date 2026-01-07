/*
 * Copyright (c) 2025, Microsoft, Inc. All rights reserved.
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

/*
 * @test
 * @requires vm.cds.supports.aot.class.linking
 * @requires vm.cds.write.archived.java.heap
 * @summary Sanity test for Jcmd AOT.end_recording command
 * @library /test/lib
 * @build JcmdAOTEndTrainingTest
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar LingeredApp.jar
 *              jdk/test/lib/apps/LingeredApp
 *              jdk/test/lib/apps/LingeredApp$1
 *              jdk/test/lib/apps/LingeredApp$SteadyStateLock
 *              jdk/test/lib/process/OutputBuffer
 * @run driver JcmdAOTEndTrainingTest
 */

import jdk.test.lib.JDKToolLauncher;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.apps.LingeredApp;
import jdk.test.lib.dcmd.PidJcmdExecutor;
import jdk.test.lib.process.OutputAnalyzer;
import java.io.IOException;

public class JcmdAOTEndTrainingTest {
    public static void main(String[] args)  throws Exception {
        test(false);
        test(true);
    }

    static void test(boolean isTraining) throws Exception {
        LingeredApp theApp = null;
        try {
            theApp = new LingeredApp();
            theApp.setUseDefaultClasspath(false);
            if (isTraining) {
                LingeredApp.startApp(theApp,
                                     "-cp", "LingeredApp.jar",
                                     "-XX:AOTMode=record",
                                     "-XX:AOTConfiguration=LingeredApp.aotconfig");
            } else {
                LingeredApp.startApp(theApp,
                                     "-cp", "LingeredApp.jar");
            }
            long pid = theApp.getPid();

            JDKToolLauncher jcmd = JDKToolLauncher.createUsingTestJDK("jcmd");
            jcmd.addToolArg(String.valueOf(pid));
            jcmd.addToolArg("AOT.end_training");

            try {
                OutputAnalyzer output = ProcessTools.executeProcess(jcmd.getCommand());
                if (isTraining) {
                    output.shouldContain("Training ended successfully");
                } else {
                    // this message is output when the VM is not recording AOT data
                    output.shouldContain("Error! Not a training run");
                }
                output.shouldHaveExitValue(0);
            } catch (Exception e) {
                throw new RuntimeException("Test failed: " + e);
            }
        }
        catch (IOException e) {
            throw new RuntimeException("Test failed: " + e);
        }
        finally {
            LingeredApp.stopApp(theApp);
        }
    }
}
