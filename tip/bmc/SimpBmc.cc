/**************************************************************************************[SimpBmc.cc]
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

#include "minisat/simp/SimpSolver.h"
#include "mcl/CircPrelude.h"
#include "mcl/Clausify.h"
#include "tip/bmc/Bmc.h"

namespace Tip {

using namespace Minisat;

//=================================================================================================
// Some helpers:
//

typedef vec<Var> LIFrame;

class SimpUnroller {
    const TipCirc& tip;
    SimpSolver&    solver;
    vec<Lit>       flop_front;

    void initialize();
public:
    SimpUnroller(const TipCirc& t, vec<LIFrame>& ui, SimpSolver& solver);
    void operator()(Clausifyer<SimpSolver>& unroll_cl);

    vec<LIFrame>&  unroll_inps;
};


SimpUnroller::SimpUnroller(const TipCirc& t, vec<LIFrame>& ui, SimpSolver& s) 
    : tip(t), solver(s), unroll_inps(ui) { initialize(); }


void SimpUnroller::initialize()
{
    // Clausify initial circuit:
    Clausifyer<SimpSolver> cl(tip.init, solver);
    for (int i = 0; i < tip.flps.size(); i++){
        Lit l = cl.clausify(tip.flps.init(tip.flps[i]));
        // solver.setFrozen(var(l), true);
        solver.freezeVar(var(l));
        flop_front.push(l);
    }

    // Extract input variables:
    unroll_inps.push();
    for (InpIt iit = tip.init.inpBegin(); iit != tip.init.inpEnd() ; ++iit){
        Gate     inp = *iit;
        uint32_t num = tip.init.number(inp);
        Lit      l   = cl.lookup(inp);
        assert(!sign(l));
        assert(num != UINT32_MAX);
        unroll_inps.last().growTo(num+1, var_Undef);
        unroll_inps.last()[num] = var(l);
    }
}


void SimpUnroller::operator()(Clausifyer<SimpSolver>& unroll_cl){
    unroll_cl.clear();
    // Bind old flop outputs to new flop inputs:
    for (int i = 0; i < tip.flps.size(); i++){
        assert(!solver.isEliminated(var(flop_front[i])));
        //solver.setFrozen(var(flop_front[i]), false);
        unroll_cl.clausifyAs(tip.flps[i], flop_front[i]);
    }

    // Clausify flop definitions:
    for (int i = 0; i < tip.flps.size(); i++){
        Lit l = unroll_cl.clausify(tip.flps.next(tip.flps[i]));
        //solver.setFrozen(var(l), true);
        solver.freezeVar(var(l));
        flop_front[i] = l;
    }

    // Clausify properties:
    for (int j = 0; j < tip.all_props.size(); j++){
        Property p = tip.all_props[j];
        if (tip.properties.propType(p) != ptype_Safety || tip.properties.propStatus(p) != pstat_Unknown)
            continue;
        Lit l = unroll_cl.clausify(tip.properties.propSig(p));
        solver.freezeVar(var(l));
    }

    // Extract input variables:
    unroll_inps.push();
    for (TipCirc::InpIt iit = tip.inpBegin(); iit != tip.inpEnd(); ++iit){
        Gate     inp = *iit;
        uint32_t num = tip.main.number(inp);
        Lit      l   = unroll_cl.lookup(inp);
        assert(!sign(l));
        assert(num != UINT32_MAX);
        unroll_inps.last().growTo(num+1, var_Undef);
        unroll_inps.last()[num] = var(l);
    }
}


//=================================================================================================
// Implementation of Simplifying BMC:
//

void simpBmc(TipCirc& tip, uint32_t begin_cycle, uint32_t stop_cycle)
{
    vec<LIFrame>           ui;                  // Unrolled set of input frames.
    SimpSolver             s;                   // SAT-solver.
    SimpUnroller           unroll(tip, ui, s);  // Unroller-helper object.
    Clausifyer<SimpSolver> ucl(tip.main, s);    // Reusable clausifyer.
    vec<Var>               frozen_vars;         // Reusable list of frozen variables.

    //s.verbosity = 1;
    for (uint32_t i = 0; i < stop_cycle; i++){
        unroll(ucl);
        //printf(" ... unrolling cycle %d\n", i);

        if (i < begin_cycle)
            continue;

        // Do CNF-level simplification:
        s.eliminate();

        // Do SAT-tests:
        int unresolved_safety = 0;
        for (int j = 0; j < tip.all_props.size(); j++){
            Property p = tip.all_props[j];
            if (tip.properties.propType(p) != ptype_Safety || tip.properties.propStatus(p) != pstat_Unknown)
                continue;
            
            Lit plit = ucl.lookup(tip.properties.propSig(p));
            printf(" --- cycle=%3d, vars=%8.3g, clauses=%8.3g, conflicts=%8.3g\n", i, (double)s.nFreeVars(), (double)s.nClauses(), (double)s.conflicts);

            //printf(" ... testing property %d\n", p);
            if (s.solve(~plit, false, false)){
                // Property falsified, create and extract trace:
                Trace             cex    = tip.traces.newTrace();
                vec<vec<lbool> >& frames = tip.traces.getFrames(cex);
                for (int k = 0; k < unroll.unroll_inps.size(); k++){
                    frames.push();
                    for (int l = 0; l < unroll.unroll_inps[k].size(); l++)
                        if (unroll.unroll_inps[k][l] != var_Undef)
                            frames.last().push(s.modelValue(unroll.unroll_inps[k][l]));
                        else
                            frames.last().push(l_Undef);
                }
                printf (" ... property falsified, created trace = %d of length %d.\n", cex, frames.size());
                tip.properties.setPropFalsified(p, cex);
            }else{
                unresolved_safety++;
                //printf (" ... property true.\n");
                assert(s.value(plit) == l_True);
            }
        }

        // Thaw all frozen variables:
        s.thaw();

        // Terminate if all safety properties resolved:
        if (unresolved_safety == 0)
            break;
    }
    printf(" --- done, vars=%8.3g, clauses=%8.3g, conflicts=%8.3g\n", (double)s.nFreeVars(), (double)s.nClauses(), (double)s.conflicts);
    s.printStats();
}

};
