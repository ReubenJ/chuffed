#include <cstdio>
#include <algorithm>
#include <cassert>
#include <chuffed/core/options.h>
#include <chuffed/core/engine.h>
#include <chuffed/core/sat.h>
#include <chuffed/core/propagator.h>
#include <chuffed/mip/mip.h>
#include <chuffed/parallel/parallel.h>

#include <iostream>
#include <sstream>
#include <fstream>

#define PRINT_ANALYSIS 0

SAT sat;

std::map<int,std::string> litString;

std::map<int,string> learntClauseString;
std::ofstream learntStatsStream;

cassert(sizeof(Lit) == 4);
cassert(sizeof(Clause) == 4);
cassert(sizeof(WatchElem) == 8);
cassert(sizeof(Reason) == 8);

//---------
// inline methods



inline void SAT::insertVarOrder(int x) {
	if (!order_heap.inHeap(x) && flags[x].decidable) order_heap.insert(x);
}

inline void SAT::setConfl(Lit p, Lit q) {
	(*short_confl)[0] = p;
	(*short_confl)[1] = q;
	confl = short_confl;
}

inline void SAT::untrailToPos(vec<Lit>& t, int p) {
	int dl = decisionLevel();

	for (int i = t.size(); i-- > p; ) {
		int x = var(t[i]);
		assigns[x] = toInt(l_Undef);
#if PHASE_SAVING
		if (so.phase_saving >= 1 || (so.phase_saving == 1 && dl >= l0))
			polarity[x] = sign(t[i]);
#endif
		insertVarOrder(x);
	}
	t.resize(p);
}

//---------
// main methods


SAT::SAT() :
		lit_sort(trailpos)
	, pushback_time(duration::zero())
	, trail(1)
	, qhead(1,0)
	, rtrail(1)
	, confl(NULL)
	, var_inc(1)
	, cla_inc(1)
	, order_heap(VarOrderLt(activity))
	, bin_clauses(0)
	, tern_clauses(0)
	, long_clauses(0)
	, learnt_clauses(0)
	, propagations(0)
	, back_jumps(0)
	, ewma_back_jumps(0)
	, nrestarts(0)
	, next_simp_db(100000)
	, clauses_literals(0)
	, learnts_literals(0)
	, max_literals(0)
	, tot_literals(0)
	, avg_depth(100)
	, confl_rate(1000)
	, ll_time(chuffed_clock::now())
	, ll_inc(1)
	, learnt_len_el(10)
	, learnt_len_occ(MAX_SHARE_LEN,learnt_len_el*1000/MAX_SHARE_LEN)
{
	newVar(); enqueue(Lit(0,1));
	newVar(); enqueue(Lit(1,0));
	temp_sc = (SClause*) malloc(TEMP_SC_LEN * sizeof(int));
	short_expl = (Clause*) malloc(sizeof(Clause) + 3 * sizeof(Lit));
	short_confl = (Clause*) malloc(sizeof(Clause) + 2 * sizeof(Lit));
	short_expl->clearFlags();
	short_confl->clearFlags();
	short_confl->sz = 2;
}

SAT::~SAT() {
	for (int i = 0; i < clauses.size(); i++) free(clauses[i]);
	for (int i = 0; i < learnts.size(); i++) free(learnts[i]);
}

void SAT::init() {
	orig_cutoff = nVars();
	ivseen.growTo(engine.vars.size(), false);
}

int SAT::newVar(int n, ChannelInfo ci) {
	int s = assigns.size();

	watches  .growBy(n);
	watches  .growBy(n);
	assigns  .growBy(n, toInt(l_Undef));
	reason   .growBy(n, NULL);
	trailpos .growBy(n, -1);
	seen     .growBy(n, 0);
	activity .growBy(n, 0);
	polarity .growBy(n, 1);
	flags    .growBy(n, 7);

	for (int i = 0; i < n; i++) {
		c_info.push(ci);
		ci.val++;
		insertVarOrder(s+i);
	}

	return s;
}

int SAT::getLazyVar(ChannelInfo ci) {
	int v;
	if (var_free_list.size()) {
		v = var_free_list.last();
		var_free_list.pop();
		fprintf(stderr, "reuse %d\n", v);
		assert(assigns[v] == toInt(l_Undef));
		assert(watches[2*v].size() == 0);
		assert(watches[2*v+1].size() == 0);
		assert(num_used[v] == 0);
		c_info[v] = ci;
		activity[v] = 0;
		polarity[v] = 1;
		flags[v] = 7;
	} else {
		v = newVar(1, ci);
		num_used.push(0);
	}
//	flags[v].setDecidable(false);
	return v;
}

void SAT::removeLazyVar(int v) {
	return;
	ChannelInfo& ci = c_info[v];
	assert(assigns[v] == toInt(l_Undef));
	assert(watches[2*v].size() == 0);
	assert(watches[2*v+1].size() == 0);
	fprintf(stderr, "free %d\n", v);
	var_free_list.push(v);
	if (ci.cons_type == 1) {
		((IntVarLL*) engine.vars[ci.cons_id])->freeLazyVar(ci.val);
	} else if (ci.cons_type == 2) {
		engine.propagators[ci.cons_id]->freeLazyVar(ci.val);
	} else NEVER;
}

void SAT::addClause(Lit p, Lit q) {
	if (value(p) == l_True || value(q) == l_True) return;
	if (value(p) == l_False && value(q) == l_False) {
		assert(false);
		TL_FAIL();
	}
	if (value(p) == l_False) {
		assert(decisionLevel() == 0);
		enqueue(q);
		return;
	}
	if (value(q) == l_False) {
		assert(decisionLevel() == 0);
		enqueue(p);
		return;
	}
	bin_clauses++;
	watches[toInt(~p)].push(q);
	watches[toInt(~q)].push(p);
}

void SAT::addClause(vec<Lit>& ps, bool one_watch) {
	int i, j;
	for (i = j = 0; i < ps.size(); i++) {
		if (value(ps[i]) == l_True) return;
		if (value(ps[i]) == l_Undef) ps[j++] = ps[i];
	}
	ps.resize(j);
	if (ps.size() == 0) {
		assert(false);
		TL_FAIL();
	}
	addClause(*Clause_new(ps), one_watch);
}

void SAT::addClause(Clause& c, bool one_watch) {
	assert(c.size() > 0);
	if (c.size() == 1) {
		assert(decisionLevel() == 0);
		if (DEBUG) fprintf(stderr, "warning: adding length 1 clause!\n");
		if (value(c[0]) == l_False) TL_FAIL();
		if (value(c[0]) == l_Undef) enqueue(c[0]);
		free(&c);
		return;
	}
	if (!c.learnt) {
		if (c.size() == 2) bin_clauses++;
		else if (c.size() == 3) tern_clauses++;
		else long_clauses++;
	}

	// Mark lazy lits which are used
	if (c.learnt) for (int i = 0; i < c.size(); i++) incVarUse(var(c[i]));

	if (c.size() == 2 && ((!c.learnt) || (so.bin_clause_opt))) {
		if (!one_watch) watches[toInt(~c[0])].push(c[1]);
		watches[toInt(~c[1])].push(c[0]);
		if (!c.learnt) free(&c);
		return;
	}
	if (!one_watch) watches[toInt(~c[0])].push(&c);
	watches[toInt(~c[1])].push(&c);
	if (c.learnt) learnts_literals += c.size();
	else            clauses_literals += c.size();
	if (c.learnt) {
          learnts.push(&c);
          if (so.learnt_stats) {
            std::set<int> levels;
            for (int i = 0 ; i < c.size() ; i++) {
              levels.insert(out_learnt_level[i]);
            }
            std::stringstream s;
            //            s << "learntclause,";
            s << c.clauseID() << "," << c.size() << "," << levels.size();
            if (so.learnt_stats_nogood) {
                s << ",";
                for (int i = 0 ; i < c.size() ; i++) {
                    s << (i == 0 ? "" : " ") << getLitString(toInt(c[i]));
              //              s << " (" << out_learnt_level[i] << ")";
                }
            }
            //std::cerr << "\n";
            learntClauseString[c.clauseID()] = s.str();
          }
        } else {
          clauses.push(&c);
        }
}

void SAT::removeClause(Clause& c) {
	assert(c.size() > 1);
	watches[toInt(~c[0])].remove(&c);
	watches[toInt(~c[1])].remove(&c);
	if (c.learnt) learnts_literals -= c.size();
	else          clauses_literals -= c.size();

	if (c.learnt) for (int i = 0; i < c.size(); i++) decVarUse(var(c[i]));

        if (c.learnt) {
            //            learntClauseScore[c.clauseID()] = c.rawActivity();
            /* if (so.debug) { */
            if (so.learnt_stats) {
                int id = c.clauseID();
                learntStatsStream << learntClauseString[id];
                learntStatsStream << ",";
                learntStatsStream << c.rawActivity();
                learntStatsStream << "\n";
                /* std::cerr << "clausescore," <<  << "," << c.rawActivity() << "\n"; */
            }
            /* } */
        }

	free(&c);
}


void SAT::topLevelCleanUp() {
  assert(decisionLevel() == 0);

	for (int i = rtrail[0].size(); i-- > 0; ) free(rtrail[0][i]);
	rtrail[0].clear();

	if (so.sat_simplify && propagations >= next_simp_db) simplifyDB();

	for (int i = 0; i < trail[0].size(); i++) {
            if (so.debug) {
                std::cerr << "setting true at top-level: " << getLitString(toInt(trail[0][i])) << "\n";
            }
        seen[var(trail[0][i])] = true;
        trailpos[var(trail[0][i])] = -1;
    }
	trail[0].clear();
	qhead[0] = 0;

}

void SAT::simplifyDB() {
	int i, j;
	for (i = j = 0; i < learnts.size(); i++) {
		if (simplify(*learnts[i])) removeClause(*learnts[i]);
		else learnts[j++] = learnts[i];
	}
  learnts.resize(j);
	next_simp_db = propagations + clauses_literals + learnts_literals;
}

bool SAT::simplify(Clause& c) {
	if (value(c[0]) == l_True) return true;
	if (value(c[1]) == l_True) return true;
	int i, j;
	for (i = j = 2; i < c.size(); i++) {
		if (value(c[i]) == l_True) return true;
		if (value(c[i]) == l_Undef) c[j++] = c[i];
	}
  c.resize(j);
	return false;
}

string showReason(Reason r) {
  std::stringstream ss;
  switch (r.d.type) {
  case 0:
    if (r.pt == NULL) {
      ss << "no reason";
    } else {
      Clause& c = *r.pt;
      ss << "clause";
      for (int i = 0 ; i < c.size() ; i++) {
        ss << " " << getLitString(toInt(~c[i]));
      }
    }
    break;
  case 1: ss << "absorbed binary clause?"; break;
  case 2: ss << "single literal " << getLitString(toInt(~toLit(r.d.d1))); break;
  case 3: ss << "two literals " << getLitString(toInt(~toLit((r.d.d1)))) << " & " << getLitString(toInt(~toLit((r.d.d2)))); break;
  }
  return ss.str();
}

// Use cases:
// enqueue from decision   , value(p) = u  , r = NULL , channel
// enqueue from analyze    , value(p) = u  , r != NULL, channel
// enqueue from unit prop  , value(p) = u  , r != NULL, channel

void SAT::enqueue(Lit p, Reason r) {
  /* if (so.debug) { */
  /*   std::cerr << "enqueue literal " << getLitString(toInt(p)) << " because " << showReason(r) << "\n"; */
  /* } */
	assert(value(p) == l_Undef);
	int v = var(p);
	assigns [v] = toInt(lbool(!sign(p)));
	trailpos[v] = engine.trailPos();
	reason  [v] = r;
	trail.last().push(p);
	ChannelInfo& ci = c_info[v];
	if (ci.cons_type == 1) engine.vars[ci.cons_id]->channel(ci.val, ci.val_type, sign(p));
}

// enqueue from FD variable, value(p) = u/f, r = ?, don't channel

void SAT::cEnqueue(Lit p, Reason r) {
  /* if (so.debug) { */
  /*   std::cerr << "c-enqueue literal " << getLitString(toInt(p)) << " because " << showReason(r) << "\n"; */
  /* } */
	assert(value(p) != l_True);
	int v = var(p);
	if (value(p) == l_False) {
		if (so.lazy) {
			if (r == NULL) {
				assert(decisionLevel() == 0);
				setConfl();
			} else {
				confl = getConfl(r, p);
				(*confl)[0] = p;
			}
		} else setConfl();
		return;
	}
	assigns [v] = toInt(lbool(!sign(p)));
	trailpos[v] = engine.trailPos();
	reason  [v] = r;
	trail.last().push(p);
}


void SAT::aEnqueue(Lit p, Reason r, int l) {
  if (so.debug) {
    std::cerr << "a-enqueue literal " << getLitString(toInt(p)) << " because " << showReason(r) << " and l=" << l << "\n";
  }
	assert(value(p) == l_Undef);
	int v = var(p);
	assigns [v] = toInt(lbool(!sign(p)));
	trailpos[v] = engine.trail_lim[l]-1;
	reason  [v] = r;
	trail[l].push(p);
}

void SAT::btToLevel(int level) {
#if DEBUG_VERBOSE
  std::cerr << "SAT::btToLevel( " << level << ")\n";
#endif
  if (decisionLevel() <= level) return;

	for (int l = trail.size(); l-- > level+1; ) {
		untrailToPos(trail[l], 0);
		for (int i = rtrail[l].size(); i--; ) {
			free(rtrail[l][i]);
		}
	}
  trail.resize(level+1);
	qhead.resize(level+1);
	rtrail.resize(level+1);

	engine.btToLevel(level);
	if (so.mip) mip->btToLevel(level);

}

void SAT::btToPos(int sat_pos, int core_pos) {
	untrailToPos(trail.last(), sat_pos);
	engine.btToPos(core_pos);
}


// Propagator methods:

bool SAT::propagate() {
	int num_props = 0;

	int& qhead = this->qhead.last();
	vec<Lit>& trail = this->trail.last();

	while (qhead < trail.size()) {
		num_props++;

		Lit p = trail[qhead++];          // 'p' is enqueued fact to propagate.
		vec<WatchElem>& ws = watches[toInt(p)];

		if (ws.size() == 0) continue;

		WatchElem *i, *j, *end;

		for (i = j = ws, end = i + ws.size(); i != end; ) {
			WatchElem& we = *i;
			switch (we.d.type) {
			case 1: {
				// absorbed binary clause
				*j++ = *i++;
				Lit q = toLit(we.d.d2);
				switch (toInt(value(q))) {
					case 0: enqueue(q, ~p); break;
					case -1:
						setConfl(q, ~p);
						qhead = trail.size();
						while (i < end) *j++ = *i++;
						break;
					default:;
				}
				continue;
			}
			case 2: {
				// wake up FD propagator
				*j++ = *i++;
				engine.propagators[we.d.d2]->wakeup(we.d.d1, 0);
				continue;
			}
			default:
				Clause& c = *we.pt;
				i++;

				// Check if already satisfied
				if (value(c[0]) == l_True || value(c[1]) == l_True) {
					*j++ = &c;
					continue;
				}

				Lit false_lit = ~p;

				// Make sure the false literal is data[1]:
				if (c[0] == false_lit) c[0] = c[1], c[1] = false_lit;

				// Look for new watch:
				for (int k = 2; k < c.size(); k++)
					if (value(c[k]) != l_False) {
						c[1] = c[k]; c[k] = false_lit;
						watches[toInt(~c[1])].push(&c);
						goto FoundWatch;
					}

				// Did not find watch -- clause is unit under assignment:
				*j++ = &c;
				if (value(c[0]) == l_False) {
					confl = &c;
					qhead = trail.size();
					while (i < end)	*j++ = *i++;
				} else {
					enqueue(c[0], &c);
				}
				FoundWatch:;
			}
		}
		ws.shrink(i-j);
	}
	propagations += num_props;

	return (confl == NULL);
}

struct activity_lt { bool operator() (Clause* x, Clause* y) { return x->activity() < y->activity(); } };
void SAT::reduceDB() {
  int i, j;

	std::sort((Clause**) learnts, (Clause**) learnts + learnts.size(), activity_lt());

  for (i = j = 0; i < learnts.size()/2; i++) {
		if (!locked(*learnts[i])) removeClause(*learnts[i]);
		else learnts[j++] = learnts[i];
  }
  for (; i < learnts.size(); i++) {
		learnts[j++] = learnts[i];
  }
  learnts.resize(j);

	if (so.verbosity >= 1) printf("%% Pruned %d learnt clauses\n", i-j);
}

std::string showClause(Clause& c) {
  std::stringstream ss;
  for (int i = 0 ; i < c.size() ; i++)
    ss << " " << getLitString(toInt(c[i]));
  return ss.str();
}

struct raw_activity_gt { bool operator() (Clause* x, Clause* y) { return x->rawActivity() > y->rawActivity(); } };
// This is wrong, because probably most of the clauses have been
// removed by the time we do this.
void SAT::printLearntStats() {
  /* std::ofstream clausefile("clause-info.csv"); */
  /* for (int i = 0 ; i < learnts.size() ; i++) { */
  /*   clausefile << learnts[i]->clauseID() << "," << learnts[i]->rawActivity() << "," << showClause(*learnts[i]) << "\n"; */
  /* } */

  std::sort((Clause**) learnts, (Clause**) learnts + learnts.size(), raw_activity_gt());
  std::cerr << "top ten clauses:\n";
  for (int i = 0 ; i < 10 && i < learnts.size() ; i++) {
    std::cerr << i << ": " << learnts[i]->rawActivity() << " " << showClause(*learnts[i]) << "\n";
  }

}

void SAT::printStats() {
	printf("%d,", bin_clauses);
	printf("%d,", tern_clauses);
	printf("%d,", long_clauses);
	printf("%.2f,", long_clauses ? (double) (clauses_literals - 3*tern_clauses) / long_clauses : 0);
	printf("%d,", learnts.size());
	printf("%.2f,", learnts.size() ? (double) learnts_literals / learnts.size() : 0);
	printf("%lld,", propagations);
	printf("%lld,", nrestarts);
	if (so.ldsb) {
		printf("%%%%%%mzn-stat: pushbackTime=%.3f\n", to_sec(pushback_time));
	}
	// Computes the average, max and median raw activity of the top 10 clauses.
	std::sort((Clause**) learnts, (Clause**) learnts + learnts.size(), raw_activity_gt());
	int cumulative_activity = 0;
	int median_activity = -1;
	int max_activity = 0;
	for (int i = 0; i < 10 && i < learnts.size(); i++){
		int rawActivity = learnts[i]->rawActivity();
		if (rawActivity >= max_activity){
			max_activity = rawActivity;
		}
		
		if (i == 4){
			median_activity = (learnts[i+1]->rawActivity() + rawActivity) / 2;
		}
		
		cumulative_activity += rawActivity;
	}
	cumulative_activity /= 10;
	printf("%d,", cumulative_activity);
	printf("%d,", max_activity);
	printf("%d,", median_activity);
}


//-----
// Branching methods

bool SAT::finished() {
	assert(so.vsids);
	while (!order_heap.empty()) {
		int x = order_heap[0];
		if (!assigns[x] && flags[x].decidable) return false;
		order_heap.removeMin();
	}
	return true;
}

DecInfo* SAT::branch() {
	if (!so.vsids) return NULL;

	assert(!order_heap.empty());

	int next = order_heap.removeMin();

	assert(!assigns[next]);
	assert(flags[next].decidable);

	return new DecInfo(NULL, 2*next+polarity[next]);
}

//-----
// Parallel methods

void SAT::updateShareParam() {
	so.share_param = 16;
/*
	double bmax = so.bandwidth / so.num_threads;
	double bsum = 0;
//	printf("Update share param\n");
	double factor = learnt_len_el * (ll_inc-0.5);
	for (int i = 0; i < MAX_SHARE_LEN; i++) {
		double lps = learnt_len_occ[i]/factor*i;
//		printf("%.3f, ", lps);
		if (bsum + lps > bmax) {
			so.share_param = i-1 + (bmax - bsum) / lps;
			if (so.share_param < 1) so.share_param = 1;
			return;
		}
		bsum += lps;
	}
	so.share_param = MAX_SHARE_LEN;
//	if (rand()%100 == 0) printf("share param = %.1f\n", so.share_param);
*/
}
