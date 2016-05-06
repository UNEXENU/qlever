// Copyright 2015, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Author: Björn Buchhold (buchhold@informatik.uni-freiburg.de)

#include <algorithm>
#include "./QueryPlanner.h"
#include "IndexScan.h"
#include "Join.h"
#include "Sort.h"
#include "OrderBy.h"
#include "Distinct.h"
#include "Filter.h"
#include "TextOperationForEntities.h"
#include "TextOperationForContexts.h"
#include "TextOperationWithoutFilter.h"
#include "TextOperationWithFilter.h"

// _____________________________________________________________________________
QueryPlanner::QueryPlanner(QueryExecutionContext* qec) : _qec(qec) { }

// _____________________________________________________________________________
QueryExecutionTree QueryPlanner::createExecutionTree(
    const ParsedQuery& pq) const {

  LOG(DEBUG) << "Creating execution plan.\n";
  // Strategy:
  // Create a graph.
  // Each triple corresponds to a node, there is an edge between two nodes iff
  // they share a variable.

  TripleGraph tg = createTripleGraph(pq);

  // Each node/triple corresponds to a scan (more than one way possible),
  // each edge corresponds to a possible join.

  // Enumerate and judge possible query plans using a DP table.
  // Each ExecutionTree for a sub-problem gives an estimate.
  // Start bottom up, i.e. with the scans for triples.
  // Always merge two solutions from the table by picking one possible join.
  // A join is possible, if there is an edge between the results.
  // Therefore we keep track of all edges that touch a sub-result.
  // When joining two sub-results, the results edges are those that belong
  // to exactly one of the two input sub-trees.
  // If two of them have the same target, only one out edge is created.
  // All edges that are shared by both subtrees, are checked if they are covered
  // by the join or if an extra filter/select is needed.

  // The algorithm then creates all possible plans for 1 to n triples.
  // To generate a plan for k triples, all subsets between i and k-i are
  // joined.

  // Filters are now added to the mix when building execution plans.
  // Without them, a plan has an execution tree and a set of
  // covered triple nodes.
  // With them, it also has a set of covered filters.
  // A filter can be applied as soon as all variables that occur in the filter
  // Are covered by the query. This is also always the place where this is done.

  // TODO: resolve cyclic queries and turn them into filters.
  // Copy made so that something can be added for cyclic queries.
  // tg.turnCyclesIntoFilters(filters);

  // Text operations from cliques (all triples connected via the context cvar).
  // Detect them and turn them into nodes with stored word part and
  // edges to connected variables.
  tg.collapseTextCliques();
  vector<vector<SubtreePlan>> finalTab;

  // Each text operation has two ways how it can be used.
  // 1) As leave in the bottom row of the tab.
  // According to the number of connected varibales, the operation creates
  // a cross product with n entities that can be used in subsequent joins.
  // 2) as intermediate unary (downwards) nodes in the execution tree.
  // This is a bit similar to sorts: they can be applied after each step
  // and will filter on one variable.
  // Cycles have to be avoided (by previously removing a triple and using it
  // as a filter later on).

  // Deal with pure text queries
  if (tg.isPureTextQuery()) {
    SubtreePlan plan = pureTextQuery(tg);
    vector<SubtreePlan> oneElementRow;
    oneElementRow.emplace_back(plan);
    finalTab.emplace_back(oneElementRow);
  } else {
    finalTab = fillDpTab(tg, pq._filters);
  }

  // If there is an order by clause, add another row to the table and
  // just add an order by / sort to every previous result if needed.
  // If the ordering is perfect already, just copy the plan.
  if (pq._orderBy.size() > 0) {
    finalTab.emplace_back(getOrderByRow(pq, finalTab));
  }

  vector<SubtreePlan>& lastRow = finalTab.back();
  AD_CHECK_GT(lastRow.size(), 0);
  size_t minCost = lastRow[0].getCostEstimate();
  size_t minInd = 0;

  for (size_t i = 1; i < lastRow.size(); ++i) {
    if (lastRow[i].getCostEstimate() < minCost) {
      minCost = lastRow[i].getCostEstimate();
      minInd = i;
    }
  }


  // A distinct modifier is applied in the end. This is very easy
  // but not necessarily optimal.
  // TODO: Adjust so that the optimal place for the operation is found.
  if (pq._distinct) {
    QueryExecutionTree distinctTree(lastRow[minInd]._qet);
    vector<size_t> keepIndices;
    for (const auto& var : pq._selectedVariables) {
      if (lastRow[minInd]._qet.getVariableColumnMap().find(var) !=
          lastRow[minInd]._qet.getVariableColumnMap().end()) {
        keepIndices.push_back(
            lastRow[minInd]._qet.getVariableColumnMap().find(
                var)->second);
      }
    }
    Distinct distinct(_qec, lastRow[minInd]._qet, keepIndices);
    distinctTree.setOperation(QueryExecutionTree::DISTINCT, &distinct);
    return distinctTree;
  }

  lastRow[minInd]._qet.setTextLimit(getTextLimit(pq._textLimit));
  LOG(DEBUG) << "Done creating execution plan.\n";
  return lastRow[minInd]._qet;
}

// _____________________________________________________________________________
vector<QueryPlanner::SubtreePlan> QueryPlanner::getOrderByRow(
    const ParsedQuery& pq,
    const vector<vector<SubtreePlan>>& dpTab) const {
  const vector<SubtreePlan>& previous = dpTab[dpTab.size() - 1];
  vector<SubtreePlan> added;
  added.reserve(previous.size());
  for (size_t i = 0; i < previous.size(); ++i) {
    if (pq._orderBy.size() == 1 && !pq._orderBy[0]._desc) {
      size_t col = previous[i]._qet.getVariableColumn(
          pq._orderBy[0]._key);
      if (col == previous[i]._qet.resultSortedOn()) {
        // Already sorted perfectly
        added.push_back(previous[i]);
      } else {
        QueryExecutionTree tree(_qec);
        Sort sort(_qec, previous[i]._qet, col);
        tree.setVariableColumns(
            previous[i]._qet.getVariableColumnMap());
        tree.setOperation(QueryExecutionTree::SORT, &sort);
        tree.setContextVars(previous[i]._qet.getContextVars());
        SubtreePlan plan(_qec);
        plan._qet = tree;
        plan._idsOfIncludedNodes = previous[i]._idsOfIncludedNodes;
        plan._idsOfIncludedFilters = previous[i]._idsOfIncludedFilters;
        added.push_back(plan);
      }
    } else {
      QueryExecutionTree tree(_qec);
      vector<pair<size_t, bool>> sortIndices;
      for (auto& ord : pq._orderBy) {
        sortIndices.emplace_back(
            pair<size_t, bool>{
                previous[i]._qet.getVariableColumn(ord._key),
                ord._desc});
      }
      OrderBy ob(_qec, previous[i]._qet, sortIndices);
      tree.setVariableColumns(previous[i]._qet.getVariableColumnMap());
      tree.setOperation(QueryExecutionTree::ORDER_BY, &ob);
      tree.setContextVars(previous[i]._qet.getContextVars());
      SubtreePlan plan(_qec);
      plan._qet = tree;
      plan._idsOfIncludedNodes = previous[i]._idsOfIncludedNodes;
      plan._idsOfIncludedFilters = previous[i]._idsOfIncludedFilters;
      added.push_back(plan);
    }
  }
  return added;
}

// _____________________________________________________________________________
void QueryPlanner::getVarTripleMap(
    const ParsedQuery& pq,
    unordered_map<string, vector<SparqlTriple>>& varToTrip,
    unordered_set<string>& contextVars) const {
  for (auto& t: pq._whereClauseTriples) {
    if (isVariable(t._s)) {
      varToTrip[t._s].push_back(t);
    }
    if (isVariable(t._p)) {
      varToTrip[t._p].push_back(t);
    }
    if (isVariable(t._o)) {
      varToTrip[t._o].push_back(t);
    }

    if (t._p == IN_CONTEXT_RELATION) {
      if (isVariable(t._s) || isWords(t._o)) {
        contextVars.insert(t._s);
      }
      if (isVariable(t._o) || isWords(t._s)) {
        contextVars.insert(t._o);
      }
    }
  }
}

// _____________________________________________________________________________
bool QueryPlanner::isVariable(const string& elem) {
  return ad_utility::startsWith(elem, "?");
}

// _____________________________________________________________________________
bool QueryPlanner::isWords(const string& elem) {
  return !isVariable(elem) && elem.size() > 0 && elem[0] != '<';
}

// _____________________________________________________________________________
QueryPlanner::TripleGraph QueryPlanner::createTripleGraph(
    const ParsedQuery& query) const {
  TripleGraph tg;
  for (auto& t : query._whereClauseTriples) {
    // Add a node for the triple.
    tg._nodeStorage.emplace_back(
        TripleGraph::Node(tg._nodeStorage.size(), t));
    auto& addedNode = tg._nodeStorage.back();
    tg._nodeMap[addedNode._id] = &tg._nodeStorage.back();
    tg._adjLists.emplace_back(vector<size_t>());
    assert(tg._adjLists.size() == tg._nodeStorage.size());
    assert(tg._adjLists.size() == addedNode._id + 1);
    // Now add an edge between the added node and every node sharing a var.
    for (auto& addedNodevar : addedNode._variables) {
      for (size_t i = 0; i < addedNode._id; ++i) {
        auto& otherNode = *tg._nodeMap[i];
        if (otherNode._variables.count(addedNodevar) > 0) {
          // There is an edge between *it->second and the node with id "id".
          tg._adjLists[addedNode._id].push_back(otherNode._id);
          tg._adjLists[otherNode._id].push_back(addedNode._id);
        }
      }
    }
  }
  return tg;
}

// _____________________________________________________________________________
vector<QueryPlanner::SubtreePlan> QueryPlanner::seedWithScansAndText(
    const QueryPlanner::TripleGraph& tg) const {
  vector<SubtreePlan> seeds;
  for (size_t i = 0; i < tg._nodeMap.size(); ++i) {
    const TripleGraph::Node& node = *tg._nodeMap.find(i)->second;
    if (node._cvar.size() > 0) {
      seeds.push_back(getTextLeafPlan(node));
    } else {
      if (node._variables.size() == 0) {
        AD_THROW(ad_semsearch::Exception::BAD_QUERY,
                 "Triples should have at least one variable. Not the case in: "
                 + node._triple.asString());
      }
      if (node._variables.size() == 1) {
        // Just pick one direction, they should be equivalent.
        SubtreePlan plan(_qec);
        plan._idsOfIncludedNodes.insert(i);
        QueryExecutionTree tree(_qec);
        if (isVariable(node._triple._s)) {
          IndexScan scan(_qec, IndexScan::ScanType::POS_BOUND_O);
          scan.setPredicate(node._triple._p);
          scan.setObject(node._triple._o);
          scan.precomputeSizeEstimate();
          tree.setOperation(QueryExecutionTree::OperationType::SCAN,
                            &scan);
          tree.setVariableColumn(node._triple._s, 0);
        } else if (isVariable(node._triple._o)) {
          IndexScan scan(_qec, IndexScan::ScanType::PSO_BOUND_S);
          scan.setPredicate(node._triple._p);
          scan.setSubject(node._triple._s);
          scan.precomputeSizeEstimate();
          tree.setOperation(QueryExecutionTree::OperationType::SCAN,
                            &scan);
          tree.setVariableColumn(node._triple._o, 0);
        } else {
          // Pred variable.
          AD_THROW(ad_semsearch::Exception::NOT_YET_IMPLEMENTED,
                   "No predicate vars yet, please. Triple in question: "
                   + node._triple.asString());
        }
        plan._qet = tree;
        seeds.push_back(plan);
      }

      if (node._variables.size() == 2) {
        // Add plans for both possible scan directions.
        if (isVariable(node._triple._p)) {
          // Pred variable.
          AD_THROW(ad_semsearch::Exception::NOT_YET_IMPLEMENTED,
                   "No predicate vars yet, please. Triple in question: "
                   + node._triple.asString());
        }
        {
          SubtreePlan plan(_qec);
          plan._idsOfIncludedNodes.insert(i);
          QueryExecutionTree tree(_qec);
          IndexScan scan(_qec, IndexScan::ScanType::PSO_FREE_S);
          scan.setPredicate(node._triple._p);
          scan.precomputeSizeEstimate();
          tree.setOperation(QueryExecutionTree::OperationType::SCAN,
                            &scan);
          tree.setVariableColumn(node._triple._s, 0);
          tree.setVariableColumn(node._triple._o, 1);
          plan._qet = tree;
          seeds.push_back(plan);
        }
        {
          SubtreePlan plan(_qec);
          plan._idsOfIncludedNodes.insert(i);
          QueryExecutionTree tree(_qec);
          IndexScan scan(_qec, IndexScan::ScanType::POS_FREE_O);
          scan.setPredicate(node._triple._p);
          scan.precomputeSizeEstimate();
          tree.setOperation(QueryExecutionTree::OperationType::SCAN,
                            &scan);
          tree.setVariableColumn(node._triple._o, 0);
          tree.setVariableColumn(node._triple._s, 1);
          plan._qet = tree;
          seeds.push_back(plan);
        }
      }
      if (node._variables.size() >= 3) {
        AD_THROW(ad_semsearch::Exception::NOT_YET_IMPLEMENTED,
                 "Triples should have at most two variables. Not the case in: "
                 + node._triple.asString());
      }
    }
  }
  return seeds;
}

// _____________________________________________________________________________
QueryPlanner::SubtreePlan QueryPlanner::getTextLeafPlan(
    const QueryPlanner::TripleGraph::Node& node) const {
  SubtreePlan plan(_qec);
  plan._idsOfIncludedNodes.insert(node._id);
  QueryExecutionTree tree(_qec);
  AD_CHECK(node._wordPart.size() > 0);
  // Subtract 1 for variables.size() for the context var.
  TextOperationWithoutFilter textOp(_qec, node._wordPart,
                                    node._variables.size() - 1);
  tree.setOperation(QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER,
                    &textOp);
  unordered_map<string, size_t> vcmap;
  size_t index = 0;
  vcmap[node._cvar] = index++;
  vcmap[string("SCORE(") + node._cvar + ")"] = index++;
  for (const auto& var : node._variables) {
    if (var != node._cvar) {
      vcmap[var] = index++;
    }
  }
  tree.setVariableColumns(vcmap);
  tree.addContextVar(node._cvar);
  plan._qet = tree;
  return plan;
}

// _____________________________________________________________________________
vector<QueryPlanner::SubtreePlan> QueryPlanner::merge(
    const vector<QueryPlanner::SubtreePlan>& a,
    const vector<QueryPlanner::SubtreePlan>& b,
    const QueryPlanner::TripleGraph& tg) const {
  // TODO: Add the following features:
  // If a join is supposed to happen, always check if it happens between
  // a scan with a relatively large result size
  // esp. with an entire relation but also with something like is-a Person
  // If that is the case look at the size estimate for the other side,
  // if that is rather small, replace the join and scan by a combination.
  std::unordered_map<string, vector<SubtreePlan>> candidates;
  // Find all pairs between a and b that are connected by an edge.
  for (size_t i = 0; i < a.size(); ++i) {
    for (size_t j = 0; j < b.size(); ++j) {
      if (connected(a[i], b[j], tg)) {
        // Find join variable(s) / columns.
        auto jcs = getJoinColumns(a[i], b[j]);
        if (jcs.size() != 1) {
          // TODO: Add joins with secondary join columns.
          AD_THROW(ad_semsearch::Exception::NOT_YET_IMPLEMENTED,
                   "Joins should happen on one variable only, for now. "
                       "No cyclic queries either, currently.");
        }
        if (
            (a[i]._qet.getType() ==
             QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER &&
             b[j]._qet.getType() !=
             QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER) ||
            (a[i]._qet.getType() !=
             QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER &&
             b[j]._qet.getType() ==
             QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER)) {
          // If one of the join results is a text operation without filter
          // also consider using the other one as filter and thus
          // turning this join into a text operation with filter, instead,
          const SubtreePlan& textPlan = a[i]._qet.getType() ==
                                        QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER
                                        ? a[i] : b[j];
          const SubtreePlan& otherPlan = a[i]._qet.getType() ==
                                         QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER
                                         ? b[j] : a[i];
          size_t otherPlanJc = a[i]._qet.getType() ==
                               QueryExecutionTree::OperationType::TEXT_WITHOUT_FILTER
                               ? jcs[0][1] : jcs[0][0];
          SubtreePlan plan(_qec);
          plan._idsOfIncludedNodes = otherPlan._idsOfIncludedNodes;
          plan._idsOfIncludedNodes.insert(
              *textPlan._idsOfIncludedNodes.begin());
          QueryExecutionTree tree(_qec);
          // Subtract 1 for variables.size() for the context var.
          const TextOperationWithoutFilter& noFilter =
              *static_cast<const TextOperationWithoutFilter*>(textPlan._qet.getRootOperation());
          TextOperationWithFilter textOp(_qec, noFilter.getWordPart(),
                                         noFilter.getNofVars(), &otherPlan._qet,
                                         otherPlanJc);
          tree.setOperation(
              QueryExecutionTree::OperationType::TEXT_WITH_FILTER,
              &textOp);
          unordered_map<string, size_t> vcmap;
          // Subtract one because the entity that we filtered on
          // is provided by the filter table and still has the same place there.
          size_t colN = 2;
          string cvar = *textPlan._qet.getContextVars().begin();
          for (auto it = textPlan._qet.getVariableColumnMap().begin();
               it != textPlan._qet.getVariableColumnMap().end(); ++it) {
            if (it->first == cvar ||
                it->first == string("SCORE(") + cvar + ")") {
              vcmap[it->first] = it->second;
            } else if (otherPlan._qet.getVariableColumnMap().count(it->first) ==
                       0) {
              vcmap[it->first] = colN++;
            }
          }
          assert(colN == textPlan._qet.getResultWidth() - 1);
          for (auto it = otherPlan._qet.getVariableColumnMap().begin();
               it != otherPlan._qet.getVariableColumnMap().end(); ++it) {
            vcmap[it->first] = colN + it->second;
          }
          tree.setVariableColumns(vcmap);
          tree.setContextVars(otherPlan._qet.getContextVars());
          tree.addContextVar(cvar);
          plan._qet = tree;
          candidates[getPruningKey(plan, jcs[0][0])].emplace_back(plan);
        }
        // Check if a sub-result has to be re-sorted
        // TODO: replace with HashJoin maybe (or add variant to possible plans).
        QueryExecutionTree left(_qec);
        QueryExecutionTree right(_qec);
        if (a[i]._qet.resultSortedOn() == jcs[0][0]) {
          left = a[i]._qet;
        } else {
          // Create a sort operation.
          Sort sort(_qec, a[i]._qet, jcs[0][0]);
          left.setVariableColumns(a[i]._qet.getVariableColumnMap());
          left.setOperation(QueryExecutionTree::SORT, &sort);
        }
        if (b[j]._qet.resultSortedOn() == jcs[0][1]) {
          right = b[j]._qet;
        } else {
          // Create a sort operation.
          Sort sort(_qec, b[j]._qet, jcs[0][1]);
          right.setVariableColumns(b[j]._qet.getVariableColumnMap());
          right.setOperation(QueryExecutionTree::SORT, &sort);
        }

        // Create the join operation.
        QueryExecutionTree tree(_qec);
        Join join(_qec, left, right, jcs[0][0], jcs[0][1]);
        tree.setVariableColumns(join.getVariableColumns());
        tree.setOperation(QueryExecutionTree::JOIN, &join);
        SubtreePlan plan(_qec);
        plan._qet = tree;
        plan._idsOfIncludedFilters = a[i]._idsOfIncludedFilters;
        plan._idsOfIncludedNodes = a[i]._idsOfIncludedNodes;
        plan._idsOfIncludedNodes.insert(
            b[j]._idsOfIncludedNodes.begin(),
            b[j]._idsOfIncludedNodes.end());
        candidates[getPruningKey(plan, jcs[0][0])].emplace_back(plan);
      }
    }
  }

  // Duplicates are removed if the same triples are touched,
  // the ordering is the same. Only the best is kept then.

  // Therefore we mapped plans and use contained triples + ordering var
  // as key.
  vector<SubtreePlan> prunedPlans;
  for (auto it = candidates.begin(); it != candidates.end(); ++it) {
    size_t minCost = std::numeric_limits<size_t>::max();
    size_t minIndex = 0;
    for (size_t i = 0; i < it->second.size(); ++i) {
      if (it->second[i].getCostEstimate() < minCost) {
        minCost = it->second[i].getCostEstimate();
        minIndex = i;
      }
    }
    if (it->second.size() > 1) {
      LOG(DEBUG) << "PRUNING SOMETHING AWAY. TREES:" << std::endl;
      for (size_t i = 0; i < it->second.size(); ++i) {
        if (it->second[i].getCostEstimate() < minCost) {
          LOG(DEBUG) << it->second[i]._qet.asString() << std::endl;
          LOG(DEBUG) << "cost: " << it->second[i].getCostEstimate() <<
                     std::endl;
        }
      }
    }


    prunedPlans.push_back(it->second[minIndex]);
  }


  return prunedPlans;
}

// _____________________________________________________________________________
string QueryPlanner::TripleGraph::asString() const {
  std::ostringstream os;
  for (size_t i = 0; i < _adjLists.size(); ++i) {
    if (_nodeMap.find(i)->second->_cvar.size() == 0) {
      os << i << " " << _nodeMap.find(i)->second->_triple.asString() << " : (";
    } else {
      os << i << " {TextOP for " << _nodeMap.find(i)->second->_cvar <<
      ", wordPart: \"" << _nodeMap.find(i)->second->_wordPart << "\"} : (";
    }

    for (size_t j = 0; j < _adjLists[i].size(); ++j) {
      os << _adjLists[i][j];
      if (j < _adjLists[i].size() - 1) { os << ", "; }
    }
    os << ')';
    if (i < _adjLists.size() - 1) { os << '\n'; }
  }
  return os.str();
}

// _____________________________________________________________________________
size_t QueryPlanner::SubtreePlan::getCostEstimate() const {
  return _qet.getCostEstimate();
}

// _____________________________________________________________________________
size_t QueryPlanner::SubtreePlan::getSizeEstimate() const {
  return _qet.getSizeEstimate();
}

// _____________________________________________________________________________
bool QueryPlanner::connected(const QueryPlanner::SubtreePlan& a,
                             const QueryPlanner::SubtreePlan& b,
                             const QueryPlanner::TripleGraph& tg) const {

  auto& smaller = a._idsOfIncludedNodes.size() < b._idsOfIncludedNodes.size()
                  ? a._idsOfIncludedNodes : b._idsOfIncludedNodes;
  auto& bigger = a._idsOfIncludedNodes.size() < b._idsOfIncludedNodes.size()
                 ? b._idsOfIncludedNodes : a._idsOfIncludedNodes;

  // Check if there is overlap.
  // If so, don't consider them as properly overlapping.
  for (auto nodeId : smaller) {
    if (bigger.count(nodeId) > 0) {
      return false;
    }
  }

  for (auto nodeId : a._idsOfIncludedNodes) {
    auto& connectedNodes = tg._adjLists[nodeId];
    for (auto targetNodeId : connectedNodes) {
      if (a._idsOfIncludedNodes.count(targetNodeId) == 0 &&
          b._idsOfIncludedNodes.count(targetNodeId) > 0) {
        return true;
      }
    }
  }
  return false;
}

// _____________________________________________________________________________
vector<array<size_t, 2>> QueryPlanner::getJoinColumns(
    const QueryPlanner::SubtreePlan& a,
    const QueryPlanner::SubtreePlan& b) const {
  vector<array<size_t, 2>> jcs;
  for (auto it = a._qet.getVariableColumnMap().begin();
       it != a._qet.getVariableColumnMap().end();
       ++it) {
    auto itt = b._qet.getVariableColumnMap().find(it->first);
    if (itt != b._qet.getVariableColumnMap().end()) {
      jcs.push_back(array<size_t, 2>{{it->second, itt->second}});
    }
  }
  return jcs;
}

// _____________________________________________________________________________
string QueryPlanner::getPruningKey(const QueryPlanner::SubtreePlan& plan,
                                   size_t orderedOnCol) const {
  // Get the ordered var
  std::ostringstream os;
  for (auto it = plan._qet.getVariableColumnMap().begin();
       it != plan._qet.getVariableColumnMap().end(); ++it) {
    if (it->second == orderedOnCol) {
      os << it->first;
      break;
    }
  }
  std::set<size_t> orderedIncludedNodes;
  orderedIncludedNodes.insert(plan._idsOfIncludedNodes.begin(),
                              plan._idsOfIncludedNodes.end());
  for (size_t ind : orderedIncludedNodes) {
    os << ' ' << ind;
  }
  return os.str();
}

// _____________________________________________________________________________
void QueryPlanner::applyFiltersIfPossible(
    vector<QueryPlanner::SubtreePlan>& row,
    const vector<SparqlFilter>& filters) const {
  // Apply every filter possible.
  // It is possible when,
  // 1) the filter has not already been applied
  // 2) all variables in the filter are covered by the query so far
  for (size_t n = 0; n < row.size(); ++n) {
    const auto& plan = row[n];
    for (size_t i = 0; i < filters.size(); ++i) {
      if (plan._idsOfIncludedFilters.count(i) > 0) {
        continue;
      }
      if (plan._qet.varCovered(filters[i]._lhs) &&
          plan._qet.varCovered(filters[i]._rhs)) {
        // Apply this filter.
        SubtreePlan newPlan(_qec);
        newPlan._idsOfIncludedFilters = plan._idsOfIncludedFilters;
        newPlan._idsOfIncludedFilters.insert(i);
        newPlan._idsOfIncludedNodes = plan._idsOfIncludedNodes;
        QueryExecutionTree tree(_qec);
        Filter filter(_qec, plan._qet, filters[i]._type,
                      plan._qet.getVariableColumn(filters[i]._lhs),
                      plan._qet.getVariableColumn(filters[i]._rhs));
        tree.setVariableColumns(plan._qet.getVariableColumnMap());
        tree.setOperation(QueryExecutionTree::FILTER, &filter);
        tree.setContextVars(plan._qet.getContextVars());
        newPlan._qet = tree;
        row[n] = newPlan;
      }
    }
  }
}

// _____________________________________________________________________________
vector<vector<QueryPlanner::SubtreePlan>> QueryPlanner::fillDpTab(
    const QueryPlanner::TripleGraph& tg,
    const vector<SparqlFilter>& filters) const {

  vector<vector<SubtreePlan>> dpTab;
  dpTab.emplace_back(seedWithScansAndText(tg));
  applyFiltersIfPossible(dpTab.back(), filters);

  for (size_t k = 2; k <= tg._nodeMap.size(); ++k) {
    dpTab.emplace_back(vector<SubtreePlan>());
    for (size_t i = 1; i <= k / 2; ++i) {
      auto newPlans = merge(dpTab[i - 1], dpTab[k - i - 1], tg);
      dpTab[k - 1].insert(dpTab[k - 1].end(), newPlans.begin(),
                          newPlans.end());
      applyFiltersIfPossible(dpTab.back(), filters);
    }
  }
  return dpTab;
}


// _____________________________________________________________________________
void QueryPlanner::addOutsideText(
    vector<vector<QueryPlanner::SubtreePlan>>& planTable,
    const TripleGraph& tg,
    const unordered_map<string, vector<size_t>>& cvarToTextNodes,
    const vector<SparqlFilter>& textFilters,
    size_t textLimit) const {
  for (auto it = cvarToTextNodes.begin(); it != cvarToTextNodes.end(); ++it) {
    addOutsideText(planTable, tg, it->first, it->second, textFilters,
                   textLimit);
  }
}

// _____________________________________________________________________________
void QueryPlanner::addOutsideText(
    vector<vector<QueryPlanner::SubtreePlan>>& planTable,
    const TripleGraph& tg,
    const string& cvar,
    const vector<size_t>& cvarTextNodes,
    const vector<SparqlFilter>& textFilters,
    size_t textLimit) const {

  string wordPart;
  unordered_set<string> freeVars;
  unordered_set<string> boundVars;

  for (auto nodeId : cvarTextNodes) {
    const auto& triple = tg._nodeMap.find(nodeId)->second->_triple;
    if (isVariable(triple._s) && triple._s != cvar) {
      if (planTable.back().begin()->_qet.varCovered(triple._s)) {
        boundVars.insert(triple._s);
      } else {
        freeVars.insert(triple._s);
      }
    }
    if (isVariable(triple._o) && triple._o != cvar) {
      if (planTable.back().begin()->_qet.varCovered(triple._o)) {
        boundVars.insert(triple._o);
      } else {
        freeVars.insert(triple._o);
      }
    }
    if (!isVariable(triple._o)) {
      if (wordPart.size() == 0) {
        wordPart = triple._o;
      } else {
        // It is okay to just concat multiple parts because they refer to the
        // same CONTEXT variable. i.e. co-occurrence of all triples within
        // the same context is desired anyway.
        // For different contexts, a different cvar would have been used.
        wordPart += " " + triple._o;
      }
    }
  }

  if (wordPart.size() == 0) {
    AD_THROW(ad_semsearch::Exception::BAD_QUERY,
             "Need a word part for each text operation.");
  }

  AD_CHECK_GT(boundVars.size(), 0);
  if (boundVars.size() > 1) {
    // CASE: A cycle was broken:
    // The dpTab so far computes the solution for the non-textual part.
    // The text operation has to keep rows where all affected variables
    // occur in the same context.
    // Other than for connecting two graphs with a text operation, we
    // do not have to build cross products for matches inside a context.
    // We just filter and keep a subset of rows from the original table.
    // This may include an additional free variable (case below).
    AD_THROW(ad_semsearch::Exception::NOT_YET_IMPLEMENTED, "TODO");
    // TODO: remember there may be free variables as well!
    return;
  }

  // CASE: No cycle, 0 or some free variables in the same context.
  // At least one variable is bound (otherwise we'd have text-only).
  // Use TextOperationForEntities with 0 or more free variables.
  // For each such free var, a full cross-product is built.
  // Therefore, just remember the freeVars and proceed similar to the case
  // With exactly one affected variable.


  // Create a text operation for entities.
  AD_CHECK_EQ(boundVars.size(), 1);
  QueryExecutionTree textSubtree(_qec);
  if (freeVars.size() == 0) {
    TextOperationForEntities textOp(_qec, wordPart, textLimit);
    textSubtree.setOperation(QueryExecutionTree::TEXT_FOR_ENTITIES, &textOp);
    textSubtree.setVariableColumns(
        createVariableColumnsMapForTextOperation(cvar,
                                                 *boundVars.begin()));
  } else {
    TextOperationForEntities textOp(_qec, wordPart, textLimit, freeVars.size());
    textSubtree.setOperation(QueryExecutionTree::TEXT_FOR_ENTITIES, &textOp);
    textSubtree.setVariableColumns(
        createVariableColumnsMapForTextOperation(
            cvar, *boundVars.begin(), freeVars));
  }
  textSubtree.addContextVar(cvar);
  // If there is no other part, we're done.
  if (planTable.size() == 0) {
    planTable.push_back(vector<SubtreePlan>());
    SubtreePlan textPlan(_qec);
    textPlan._qet = textSubtree;
    textPlan._idsOfIncludedNodes.insert(cvarTextNodes.begin(),
                                        cvarTextNodes.end());
    planTable.back().push_back(textPlan);
  } else {
    // Otherwise, for each result make the combination.
    planTable.push_back(vector<SubtreePlan>());
    const auto& lastRow = planTable[planTable.size() - 2];
    for (const auto& plan : lastRow) {
      SubtreePlan combinedPlan(_qec);
      combinedPlan._idsOfIncludedNodes = plan._idsOfIncludedNodes;
      combinedPlan._idsOfIncludedNodes.insert(
          cvarTextNodes.begin(), cvarTextNodes.end());
      combinedPlan._idsOfIncludedFilters = plan._idsOfIncludedFilters;
      // Make sure the rest is sorted by that variable.
      QueryExecutionTree left(_qec);
      if (plan._qet.resultSortedOn() ==
          plan._qet.getVariableColumn(*boundVars.begin())) {
        left = plan._qet;
      } else {
        // Create a sort operation.
        Sort sort(_qec, plan._qet,
                  plan._qet.getVariableColumn(*boundVars.begin()));
        left.setVariableColumns(plan._qet.getVariableColumnMap());
        left.setOperation(QueryExecutionTree::SORT, &sort);
      }
      QueryExecutionTree right(_qec);
      Sort textSort(_qec, textSubtree,
                    textSubtree.getVariableColumn(
                        *boundVars.begin()));
      right.setVariableColumns(textSubtree.getVariableColumnMap());
      right.setOperation(QueryExecutionTree::SORT, &textSort);
      // Join the text result and the rest.
      Join join(_qec, left, right, left.resultSortedOn(),
                right.resultSortedOn());
      combinedPlan._qet.setOperation(QueryExecutionTree::JOIN, &join);
      combinedPlan._qet.setVariableColumns(join.getVariableColumns());
      combinedPlan._qet.setContextVars(left.getContextVars());
      combinedPlan._qet.addContextVar(cvar);
      planTable.back().emplace_back(combinedPlan);
    }
  }
}

// _____________________________________________________________________________
QueryPlanner::SubtreePlan QueryPlanner::pureTextQuery(
    const TripleGraph& tg) const {
  QueryExecutionTree textSubtree(_qec);
  TextOperationForContexts textOp(_qec, tg._nodeStorage.begin()->_wordPart, 1);
  textSubtree.setOperation(QueryExecutionTree::TEXT_FOR_CONTEXTS, &textOp);
  textSubtree.setVariableColumn(tg._nodeStorage.begin()->_cvar, 0);
  textSubtree.setVariableColumn(
      string("SCORE(") + tg._nodeStorage.begin()->_cvar + ")", 1);
  textSubtree.addContextVar(tg._nodeStorage.begin()->_cvar);
  SubtreePlan textPlan(_qec);
  textPlan._qet = textSubtree;
  textPlan._idsOfIncludedNodes.insert(0);
  return textPlan;
}

// _____________________________________________________________________________
size_t QueryPlanner::getTextLimit(const string& textLimitString) const {
  if (textLimitString.size() == 0) {
    return 1;
  } else {
    return static_cast<size_t>(atol(textLimitString.c_str()));
  }
}


// _____________________________________________________________________________
bool QueryPlanner::TripleGraph::isTextNode(size_t i) const {
  return _nodeMap.count(i) > 0 &&
         (_nodeMap.find(i)->second->_triple._p == IN_CONTEXT_RELATION ||
          _nodeMap.find(i)->second->_triple._p == HAS_CONTEXT_RELATION);
}

// _____________________________________________________________________________
unordered_map<string, vector<size_t>>
QueryPlanner::TripleGraph::identifyTextCliques() const {
  unordered_map<string, vector<size_t>> contextVarToTextNodesIds;
  std::set<string> contextVars;
  // Find all context vars.
  for (size_t i = 0; i < _adjLists.size(); ++i) {
    if (isTextNode(i)) {
      if (!isVariable(_nodeMap.find(i)->second->_triple._s)) {
        if (isVariable(_nodeMap.find(i)->second->_triple._o)) {
          contextVars.insert(_nodeMap.find(i)->second->_triple._o);
        } else {
          AD_THROW(ad_semsearch::Exception::BAD_QUERY,
                   "Triples need at least one variable.");
        }
      }
      if (!isVariable(_nodeMap.find(i)->second->_triple._o)) {
        if (isVariable(_nodeMap.find(i)->second->_triple._s)) {
          contextVars.insert(_nodeMap.find(i)->second->_triple._s);
        } else {
          AD_THROW(ad_semsearch::Exception::BAD_QUERY,
                   "Triples need at least one variable.");
        }
      }
    }
  }

  // Iterate again and fill contextVar -> triples map
  for (size_t i = 0; i < _adjLists.size(); ++i) {
    if (isTextNode(i)) {
      if (contextVars.count(_nodeMap.find(i)->second->_triple._s) > 0) {
        contextVarToTextNodesIds[_nodeMap.find(
            i)->second->_triple._s].push_back(i);
        AD_CHECK_EQ(0,
                    contextVars.count(_nodeMap.find(i)->second->_triple._o));
      }
      if (contextVars.count(_nodeMap.find(i)->second->_triple._o) > 0) {
        contextVarToTextNodesIds[_nodeMap.find(
            i)->second->_triple._o].push_back(i);
        AD_CHECK_EQ(0,
                    contextVars.count(_nodeMap.find(i)->second->_triple._s));
      }
    }
  }
  return contextVarToTextNodesIds;
}


// _____________________________________________________________________________
vector<pair<QueryPlanner::TripleGraph, vector<SparqlFilter>>>
QueryPlanner::TripleGraph::splitAtContextVars(
    const vector<SparqlFilter>& origFilters,
    unordered_map<string, vector<size_t>>& contextVarToTextNodes) const {
  vector<pair<QueryPlanner::TripleGraph, vector<SparqlFilter>>> retVal;
  // Recursively split the graph a context nodes.
  // Base-case: No no context nodes, return the graph itself.
  if (contextVarToTextNodes.size() == 0) {
    retVal.emplace_back(make_pair(*this, origFilters));
  } else {
    // Just take the first contextVar and split at it.
    unordered_set<size_t> textNodeIds;
    textNodeIds.insert(contextVarToTextNodes.begin()->second.begin(),
                       contextVarToTextNodes.begin()->second.end());

    // For the next iteration / recursive call(s):
    // Leave out the first one because it has been worked on in this call.
    unordered_map<string, vector<size_t>> cTMapNextIteration;
    cTMapNextIteration.insert(++contextVarToTextNodes.begin(),
                              contextVarToTextNodes.end());

    // Find a node to start the split.
    size_t startNode = 0;
    while (startNode < _adjLists.size() && textNodeIds.count(startNode) > 0) {
      ++startNode;
    }
    // If no start node was found, this means only text triples left.
    // --> don't enter code block below and return empty vector.
    if (startNode != _adjLists.size()) {
      // If we have a start node, do a BFS to obtain a set of reachable nodes
      auto reachableNodes = bfsLeaveOut(startNode, textNodeIds);
      if (reachableNodes.size() == _adjLists.size() - textNodeIds.size()) {
        // Case: cyclic or text operation was on the "outside"
        // -> only one split to work with further.
        // Recursively solve this split
        // (because there may be another context var in it)
        TripleGraph withoutText(*this, reachableNodes);
        vector<SparqlFilter> filters = pickFilters(origFilters,
                                                   reachableNodes);
        auto recursiveResult = withoutText.splitAtContextVars(filters,
                                                              cTMapNextIteration);
        retVal.insert(retVal.begin(), recursiveResult.begin(),
                      recursiveResult.end());
      } else {
        // Case: The split created two or more non-empty parts.
        // Find all parts so that the number of triples in them plus
        // the number of text triples equals the number of total triples.
        vector<vector<size_t>> setsOfReachablesNodes;
        unordered_set<size_t> nodesDone;
        nodesDone.insert(textNodeIds.begin(), textNodeIds.end());
        nodesDone.insert(reachableNodes.begin(), reachableNodes.end());
        setsOfReachablesNodes.emplace_back(reachableNodes);
        assert(nodesDone.size() < _adjLists.size());
        while (nodesDone.size() < _adjLists.size()) {
          while (startNode < _adjLists.size() &&
                 nodesDone.count(startNode) > 0) {
            ++startNode;
          }
          reachableNodes = bfsLeaveOut(startNode, textNodeIds);
          nodesDone.insert(reachableNodes.begin(), reachableNodes.end());
          setsOfReachablesNodes.emplace_back(reachableNodes);
        }
        // Recursively split each part because there may be other context vars.
        for (const auto& rNodes : setsOfReachablesNodes) {
          TripleGraph smallerGraph(*this, rNodes);
          vector<SparqlFilter> filters = pickFilters(origFilters,
                                                     rNodes);
          auto recursiveResult = smallerGraph.splitAtContextVars(
              filters,
              cTMapNextIteration);
          retVal.insert(retVal.begin(), recursiveResult.begin(),
                        recursiveResult.end());
        }
      }
    }
  }
  return retVal;
}

// _____________________________________________________________________________
vector<size_t> QueryPlanner::TripleGraph::bfsLeaveOut(
    size_t startNode,
    unordered_set<size_t> leaveOut) const {
  vector<size_t> res;
  unordered_set<size_t> visited;
  std::list<size_t> queue;
  queue.push_back(startNode);
  visited.insert(startNode);
  while (!queue.empty()) {
    size_t n = queue.front();
    queue.pop_front();
    res.push_back(n);
    auto& neighbors = _adjLists[n];
    for (size_t v : neighbors) {
      if (visited.count(v) == 0 && leaveOut.count(v) == 0) {
        visited.insert(v);
        queue.push_back(v);
      }
    }
  }
  return res;
}

// _____________________________________________________________________________
vector<SparqlFilter> QueryPlanner::TripleGraph::pickFilters(
    const vector<SparqlFilter>& origFilters,
    const vector<size_t>& nodes) const {
  vector<SparqlFilter> ret;
  unordered_set<string> coveredVariables;
  for (auto n : nodes) {
    auto& node = *_nodeMap.find(n)->second;
    coveredVariables.insert(node._variables.begin(), node._variables.end());
  }
  for (auto& f : origFilters) {
    if (coveredVariables.count(f._lhs) > 0 ||
        coveredVariables.count(f._rhs) > 0) {
      ret.push_back(f);
    }
  }
  return ret;
}

// _____________________________________________________________________________
QueryPlanner::TripleGraph::TripleGraph(const QueryPlanner::TripleGraph& other,
                                       vector<size_t> keepNodes) {
  unordered_set<size_t> keep;
  for (auto v : keepNodes) {
    keep.insert(v);
  }
  // Copy nodes to be kept and assign new node id's.
  // Keep information about the id change in a map.
  unordered_map<size_t, size_t> idChange;
  for (size_t i = 0; i < other._nodeMap.size(); ++i) {
    if (keep.count(i) > 0) {
      _nodeStorage.push_back(*other._nodeMap.find(i)->second);
      idChange[i] = _nodeMap.size();
      _nodeStorage.back()._id = _nodeMap.size();
      _nodeMap[idChange[i]] = &_nodeStorage.back();
    }
  }
  // Adjust adjacency lists accordingly.
  for (size_t i = 0; i < other._adjLists.size(); ++i) {
    if (keep.count(i) > 0) {
      vector<size_t> adjList;
      for (size_t v : other._adjLists[i]) {
        if (keep.count(v) > 0) {
          adjList.push_back(idChange[v]);
        }
      }
      _adjLists.push_back(adjList);
    }
  }
}

// _____________________________________________________________________________
QueryPlanner::TripleGraph::TripleGraph(const TripleGraph& other)
    : _adjLists(other._adjLists), _nodeMap(), _nodeStorage() {
  for (auto it = other._nodeMap.begin(); it != other._nodeMap.end(); ++it) {
    _nodeStorage.push_back(*it->second);
    _nodeMap[it->first] = &_nodeStorage.back();
  }
}

// _____________________________________________________________________________
QueryPlanner::TripleGraph& QueryPlanner::TripleGraph::operator=(
    const TripleGraph& other) {
  _adjLists = other._adjLists;
  for (auto it = other._nodeMap.begin(); it != other._nodeMap.end(); ++it) {
    _nodeStorage.push_back(*it->second);
    _nodeMap[it->first] = &_nodeStorage.back();
  }
  return *this;
}

// _____________________________________________________________________________
QueryPlanner::TripleGraph::TripleGraph() :
    _adjLists(), _nodeMap(), _nodeStorage() {

}

// _____________________________________________________________________________
void QueryPlanner::TripleGraph::collapseTextCliques() {
  // Create a map from context var to triples it occurs in (the cliques).
  std::unordered_map<string, vector<size_t>> cvarsToTextNodes(
      identifyTextCliques());
  if (cvarsToTextNodes.size() == 0) { return; }
  // Now turn each such clique into a new node the represents that whole
  // text operation clique.
  size_t id = 0;
  vector<Node> textNodes;
  unordered_map<size_t, size_t> removedNodeIds;
  vector<std::set<size_t>> tnAdjSetsToOldIds;
  for (auto it = cvarsToTextNodes.begin(); it != cvarsToTextNodes.end(); ++it) {
    auto& cvar = it->first;
    string wordPart;
    vector<SparqlTriple> trips;
    tnAdjSetsToOldIds.push_back(std::set<size_t>());
    auto& adjNodes = tnAdjSetsToOldIds.back();
    for (auto nid : it->second) {
      removedNodeIds[nid] = id;
      adjNodes.insert(_adjLists[nid].begin(), _adjLists[nid].end());
      auto& triple = _nodeMap[nid]->_triple;
      trips.push_back(triple);
      if (triple._s == cvar && !isVariable(triple._o)) {
        if (wordPart.size() > 0) {
          wordPart += " ";
        }
        wordPart += triple._o;
      }
      if (triple._o == cvar && !isVariable(triple._s)) {
        if (wordPart.size() > 0) {
          wordPart += " ";
        }
        wordPart += triple._s;
      }
    }
    textNodes.emplace_back(Node(id++, cvar, wordPart, trips));
    assert(tnAdjSetsToOldIds.size() == id);
  }

  // Finally update the graph (node ids and adj lists).
  vector<vector<size_t>> oldAdjLists = _adjLists;
  std::list<TripleGraph::Node> oldNodeStorage = _nodeStorage;
  _nodeStorage.clear();
  _nodeMap.clear();
  _adjLists.clear();
  unordered_map<size_t, size_t> idMapOldToNew;
  unordered_map<size_t, size_t> idMapNewToOld;

  // Storage and ids.
  for (auto& tn : textNodes) {
    _nodeStorage.push_back(tn);
    _nodeMap[tn._id] = &_nodeStorage.back();
  }

  for (auto& n : oldNodeStorage) {
    if (removedNodeIds.count(n._id) == 0) {
      idMapOldToNew[n._id] = id;
      idMapNewToOld[id] = n._id;
      n._id = id++;
      _nodeStorage.push_back(n);
      _nodeMap[n._id] = &_nodeStorage.back();
    }
  }

  // Adj lists
  // First for newly created text nodes.
  for (size_t i = 0; i < tnAdjSetsToOldIds.size(); ++i) {
    const auto& nodes = tnAdjSetsToOldIds[i];
    std::set<size_t> adjNodes;
    for (auto nid : nodes) {
      if (removedNodeIds.count(nid) == 0) {
        adjNodes.insert(idMapOldToNew[nid]);
      } else if (removedNodeIds[nid] != i) {
        adjNodes.insert(removedNodeIds[nid]);
      }
    }
    vector<size_t> adjList;
    adjList.insert(adjList.begin(), adjNodes.begin(), adjNodes.end());
    _adjLists.emplace_back(adjList);
  }
  assert(_adjLists.size() == textNodes.size());
  assert(_adjLists.size() == tnAdjSetsToOldIds.size());
  // Then for remaining (regular) nodes.
  for (size_t i = textNodes.size(); i < _nodeMap.size(); ++i) {
    const Node& node = *_nodeMap[i];
    const auto& oldAdjList = oldAdjLists[idMapNewToOld[node._id]];
    std::set<size_t> adjNodes;
    for (auto nid : oldAdjList) {
      if (removedNodeIds.count(nid) == 0) {
        adjNodes.insert(idMapOldToNew[nid]);
      } else {
        adjNodes.insert(removedNodeIds[nid]);
      }
    }
    vector<size_t> adjList;
    adjList.insert(adjList.begin(), adjNodes.begin(), adjNodes.end());
    _adjLists.emplace_back(adjList);
  }
}

// _____________________________________________________________________________
bool QueryPlanner::TripleGraph::isPureTextQuery() {
  return _nodeStorage.size() == 1 && _nodeStorage.begin()->_cvar.size() > 0;
}


// _____________________________________________________________________________
unordered_map<string, size_t>
QueryPlanner::createVariableColumnsMapForTextOperation(
    const string& contextVar,
    const string& entityVar,
    const unordered_set<string>& freeVars,
    const vector<pair<QueryExecutionTree, size_t>>& subtrees) {
  AD_CHECK(contextVar.size() > 0);
  unordered_map<string, size_t> map;
  size_t n = 0;
  if (entityVar.size() > 0) {
    map[entityVar] = n++;
    map[string("SCORE(") + contextVar + ")"] = n++;
    map[contextVar] = n++;
  } else {
    map[contextVar] = n++;
    map[string("SCORE(") + contextVar + ")"] = n++;
  }

  for (const auto& v : freeVars) {
    map[v] = n++;
  }

  for (size_t i = 0; i < subtrees.size(); ++i) {
    size_t offset = n;
    for (auto it = subtrees[i].first.getVariableColumnMap().begin();
         it != subtrees[i].first.getVariableColumnMap().end(); ++it) {
      map[it->first] = offset + it->second;
      ++n;
    }
  }
  return map;
}