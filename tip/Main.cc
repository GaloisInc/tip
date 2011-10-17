/*****************************************************************************************[Main.cc]
Copyright (c) 2011, Niklas Sorensson

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#include "minisat/utils/Options.h"
#include "minisat/utils/System.h"
#include "tip/TipCirc.h"
#include "tip/liveness/EmbedFairness.h"
#include "tip/liveness/Liveness.h"
#include "tip/reductions/RemoveUnused.h"
#include "tip/reductions/Substitute.h"
#include "tip/reductions/ExtractSafety.h"
#include "tip/reductions/TemporalDecomposition.h"

using namespace Minisat;
using namespace Tip;

static bool use_bad_exit = false;
static void SIGINT_exit(int)
{
    printf("\n"); printf("*** INTERRUPTED***\n");
    // TMP: should call _exit() to avoid stupid malloc deadlocks, but we do this to simplify
    // profiling of long running jobs.
    if (use_bad_exit){
        printf("*** WARNING: calling 'exit()' in signal handler. May cause dead-lock!\n");
        exit(1);
    }else
        _exit(1);
}


int main(int argc, char** argv)
{
    setlinebuf(stdout);
    setUsageHelp("USAGE: %s [options] <input-file> <result-output-file>\n\n  where input is in plain or gzipped binary AIGER.\n");
    IntOption bver ("MAIN", "bv",   "Version of BMC to be used.", 0, IntRange(0,2));
    IntOption depth("MAIN", "k",    "Maximal depth of unrolling.", INT32_MAX, IntRange(0,INT32_MAX));
    IntOption safe ("MAIN", "safe", "Which safety property to work on.", -1, IntRange(-1,INT32_MAX));
    IntOption live ("MAIN", "live", "Which liveness property to work on.", -1, IntRange(-1,INT32_MAX));
    IntOption kind ("MAIN", "kind", "What kind of algorithm to run.", 0, IntRange(0,INT32_MAX));
    IntOption verb ("MAIN", "verb", "Verbosity level.", 1, IntRange(0,10));
    IntOption sce  ("MAIN", "sce",  "Use semantic constraint extraction (0=off, 1=minimize-algorithm, 2=basic-algorithm).", 0, IntRange(0,2));
    BoolOption prof("MAIN", "prof", "(temporary) Use bad signal-handler to help gprof.", false);
    BoolOption coif("MAIN", "coif", "Use initial cone-of-influence reduction.", true);
    BoolOption td  ("MAIN", "td",   "Use temporal decomposition.", false);
    BoolOption xsafe("MAIN", "xsafe", "Extract extra safety properties.", false);
    StringOption alg("MAIN", "alg", "Main model checking algorithm to use.", "rip");
    IntOption rip_bmc("RIP", "rip-bmc", "Bmc-mode to use in Rip-engine (0=none, 1=safe, 2=live).", 1);

    parseOptions(argc, argv, true);

    if (argc < 2 || argc > 3)
        printUsageAndExit(argc, argv);

    use_bad_exit = prof;
    sigTerm(SIGINT_exit);

    TipCirc tc;
    tc.verbosity = verb;

    // Simple algorithm flow for testing:
    tc.readAiger(argv[1]);
    tc.stats();

    // Extract extra safety properties
    if (xsafe)
        extractSafety(tc);

    // Embed fairness constraints and merge "justice" signals:
    embedFairness(tc);
    tc.stats();

    // Select one safety or liveness property:
    if (safe >= 0) tc.selSafe(safe);
    if (live >= 0) tc.selLive(live);

    // Perform "cone-of-influence" reduction:
    if (coif){
        removeUnusedLogic(tc);
        tc.stats(); }

    if (sce > 0){
        tc.sce(sce == 1, false);
        tc.stats();
        substituteConstraints(tc);
        tc.stats();
        removeUnusedLogic(tc);
        tc.stats();
    }

    if (td){
        temporalDecomposition(tc);
        tc.stats();
    }

    if (strcmp(alg, "bmc") == 0)
        tc.bmc(0,depth, (TipCirc::BmcVersion)(int)bver);
    else if (strcmp(alg, "rip") == 0)
        tc.trip((RipBmcMode)(int)rip_bmc);
    else if (strcmp(alg, "live") == 0)
        checkLiveness(tc,depth);
    else if (strcmp(alg, "biere") == 0)
        checkLivenessBiere(tc,kind);
    else if (strcmp(alg, "bierebmc") == 0)
        bmcLivenessBiere(tc,kind);

    tc.printResults();

    if (argc == 3){
        FILE* res = fopen(argv[2], "w");
        if (!res) printf("ERROR! Failed to open results file: %s\n", argv[2]), exit(1);
        tc.writeResultsAiger(res);
        fclose(res);
    }

    return 0;
}
