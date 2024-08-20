//  Copyright 2022, University of Freiburg,
//                  Chair of Algorithms and Data Structures.
//  Author: Johannes Kalmbach <kalmbach@cs.uni-freiburg.de>

#include "ExportQueryExecutionTrees.h"

#include <absl/strings/str_cat.h>

#include <ranges>

#include "parser/RdfEscaping.h"
#include "util/ConstexprUtils.h"
#include "util/http/MediaTypes.h"

// _____________________________________________________________________________
namespace {
// Return a range that contains the indices of the rows that have to be exported
// from the `idTable` given the `LimitOffsetClause`. It takes into account the
// LIMIT, the OFFSET, and the actual size of the `idTable`
auto getRowIndices(const LimitOffsetClause& limitOffset, const Result& result) {
  const IdTable& idTable = result.idTable();
  return std::views::iota(limitOffset.actualOffset(idTable.size()),
                          limitOffset.upperBound(idTable.size()));
}
}  // namespace

// _____________________________________________________________________________
cppcoro::generator<QueryExecutionTree::StringTriple>
ExportQueryExecutionTrees::constructQueryResultToTriples(
    const QueryExecutionTree& qet,
    const ad_utility::sparql_types::Triples& constructTriples,
    LimitOffsetClause limitAndOffset, std::shared_ptr<const Result> res,
    CancellationHandle cancellationHandle) {
  for (size_t i : getRowIndices(limitAndOffset, *res)) {
    ConstructQueryExportContext context{i, res->idTable(), res->localVocab(),
                                        qet.getVariableColumns(),
                                        qet.getQec()->getIndex()};
    using enum PositionInTriple;
    for (const auto& triple : constructTriples) {
      auto subject = triple[0].evaluate(context, SUBJECT);
      auto predicate = triple[1].evaluate(context, PREDICATE);
      auto object = triple[2].evaluate(context, OBJECT);
      if (!subject.has_value() || !predicate.has_value() ||
          !object.has_value()) {
        continue;
      }
      co_yield {std::move(subject.value()), std::move(predicate.value()),
                std::move(object.value())};
      cancellationHandle->throwIfCancelled();
    }
  }
}

// _____________________________________________________________________________
template <>
ad_utility::streams::stream_generator ExportQueryExecutionTrees::
    constructQueryResultToStream<ad_utility::MediaType::turtle>(
        const QueryExecutionTree& qet,
        const ad_utility::sparql_types::Triples& constructTriples,
        LimitOffsetClause limitAndOffset, std::shared_ptr<const Result> result,
        CancellationHandle cancellationHandle) {
  result->logResultSize();
  auto generator =
      constructQueryResultToTriples(qet, constructTriples, limitAndOffset,
                                    result, std::move(cancellationHandle));
  for (const auto& triple : generator) {
    co_yield triple.subject_;
    co_yield ' ';
    co_yield triple.predicate_;
    co_yield ' ';
    // NOTE: It's tempting to co_yield an expression using a ternary operator:
    // co_yield triple._object.starts_with('"')
    //     ? RdfEscaping::validRDFLiteralFromNormalized(triple._object)
    //     : triple._object;
    // but this leads to 1. segfaults in GCC (probably a compiler bug) and 2.
    // to unnecessary copies of `triple._object` in the `else` case because
    // the ternary always has to create a new prvalue.
    if (triple.object_.starts_with('"')) {
      std::string objectAsValidRdfLiteral =
          RdfEscaping::validRDFLiteralFromNormalized(triple.object_);
      co_yield objectAsValidRdfLiteral;
    } else {
      co_yield triple.object_;
    }
    co_yield " .\n";
  }
}

// _____________________________________________________________________________
nlohmann::json
ExportQueryExecutionTrees::constructQueryResultBindingsToQLeverJSON(
    const QueryExecutionTree& qet,
    const ad_utility::sparql_types::Triples& constructTriples,
    const LimitOffsetClause& limitAndOffset, std::shared_ptr<const Result> res,
    CancellationHandle cancellationHandle) {
  auto generator = constructQueryResultToTriples(qet, constructTriples,
                                                 limitAndOffset, std::move(res),
                                                 std::move(cancellationHandle));
  std::vector<std::array<std::string, 3>> jsonArray;
  for (auto& triple : generator) {
    jsonArray.push_back({std::move(triple.subject_),
                         std::move(triple.predicate_),
                         std::move(triple.object_)});
  }
  return jsonArray;
}

// _____________________________________________________________________________
cppcoro::generator<std::string>
ExportQueryExecutionTrees::constructQueryResultBindingsToQLeverJSONStream(
    const QueryExecutionTree& qet,
    const ad_utility::sparql_types::Triples& constructTriples,
    const LimitOffsetClause& limitAndOffset, std::shared_ptr<const Result> res,
    CancellationHandle cancellationHandle) {
  auto generator = constructQueryResultToTriples(qet, constructTriples,
                                                 limitAndOffset, std::move(res),
                                                 std::move(cancellationHandle));
  for (auto& triple : generator) {
    auto binding = nlohmann::json::array({std::move(triple.subject_),
                                          std::move(triple.predicate_),
                                          std::move(triple.object_)});
    co_yield binding.dump();
  }
}

// _____________________________________________________________________________
// Create the row indicated by rowIndex from IdTable in QLeverJSON format.
nlohmann::json idTableToQLeverJSONRow(
    const QueryExecutionTree& qet,
    const QueryExecutionTree::ColumnIndicesAndTypes& columns,
    const LocalVocab& localVocab, const size_t rowIndex, const IdTable& data) {
  // We need the explicit `array` constructor for the special case of zero
  // variables.
  auto row = nlohmann::json::array();
  for (const auto& opt : columns) {
    if (!opt) {
      row.emplace_back(nullptr);
      continue;
    }
    const auto& currentId = data(rowIndex, opt->columnIndex_);
    const auto& optionalStringAndXsdType =
        ExportQueryExecutionTrees::idToStringAndType(qet.getQec()->getIndex(),
                                                     currentId, localVocab);
    if (!optionalStringAndXsdType.has_value()) {
      row.emplace_back(nullptr);
      continue;
    }
    const auto& [stringValue, xsdType] = optionalStringAndXsdType.value();
    if (xsdType) {
      row.emplace_back('"' + stringValue + "\"^^<" + xsdType + '>');
    } else {
      row.emplace_back(stringValue);
    }
  }
  return row;
}

// _____________________________________________________________________________
nlohmann::json ExportQueryExecutionTrees::idTableToQLeverJSONArray(
    const QueryExecutionTree& qet, const LimitOffsetClause& limitAndOffset,
    const QueryExecutionTree::ColumnIndicesAndTypes& columns,
    std::shared_ptr<const Result> result,
    CancellationHandle cancellationHandle) {
  AD_CORRECTNESS_CHECK(result != nullptr);
  const IdTable& data = result->idTable();
  nlohmann::json json = nlohmann::json::array();

  for (size_t rowIndex : getRowIndices(limitAndOffset, *result)) {
    json.emplace_back(idTableToQLeverJSONRow(qet, columns, result->localVocab(),
                                             rowIndex, data));
    cancellationHandle->throwIfCancelled();
  }
  return json;
}

// _____________________________________________________________________________
cppcoro::generator<std::string>
ExportQueryExecutionTrees::idTableToQLeverJSONBindingsStream(
    const QueryExecutionTree& qet, const LimitOffsetClause& limitAndOffset,
    const QueryExecutionTree::ColumnIndicesAndTypes columns,
    std::shared_ptr<const Result> result,
    CancellationHandle cancellationHandle) {
  AD_CORRECTNESS_CHECK(result != nullptr);
  const auto rowIndices = getRowIndices(limitAndOffset, *result);
  for (size_t rowIndex : rowIndices) {
    co_yield idTableToQLeverJSONRow(qet, columns, result->localVocab(),
                                    rowIndex, result->idTable())
        .dump();
    cancellationHandle->throwIfCancelled();
  }
}

// _____________________________________________________________________________
std::optional<std::pair<std::string, const char*>>
ExportQueryExecutionTrees::idToStringAndTypeForEncodedValue(Id id) {
  using enum Datatype;
  switch (id.getDatatype()) {
    case Undefined:
      return std::nullopt;
    case Double:
      // We use the immediately invoked lambda here because putting this block
      // in braces confuses the test coverage tool.
      return [id] {
        // Format as integer if fractional part is zero, let C++ decide
        // otherwise.
        std::stringstream ss;
        double d = id.getDouble();
        double dIntPart;
        if (std::modf(d, &dIntPart) == 0.0) {
          ss << std::fixed << std::setprecision(0) << id.getDouble();
        } else {
          ss << d;
        }
        return std::pair{std::move(ss).str(), XSD_DECIMAL_TYPE};
      }();
    case Bool:
      return id.getBool() ? std::pair{"true", XSD_BOOLEAN_TYPE}
                          : std::pair{"false", XSD_BOOLEAN_TYPE};
    case Int:
      return std::pair{std::to_string(id.getInt()), XSD_INT_TYPE};
    case Date:
      return id.getDate().toStringAndType();
    case BlankNodeIndex:
      return std::pair{absl::StrCat("_:bn", id.getBlankNodeIndex().get()),
                       nullptr};
    default:
      AD_FAIL();
  }
}

// _____________________________________________________________________________
ad_utility::triple_component::LiteralOrIri
ExportQueryExecutionTrees::getLiteralOrIriFromVocabIndex(
    const Index& index, Id id, const LocalVocab& localVocab) {
  using LiteralOrIri = ad_utility::triple_component::LiteralOrIri;
  switch (id.getDatatype()) {
    case Datatype::LocalVocabIndex:
      return localVocab.getWord(id.getLocalVocabIndex()).asLiteralOrIri();
    case Datatype::VocabIndex: {
      auto entity = index.indexToString(id.getVocabIndex());
      return LiteralOrIri::fromStringRepresentation(entity);
    }
    default:
      AD_FAIL();
  }
};

// _____________________________________________________________________________
template <bool removeQuotesAndAngleBrackets, bool onlyReturnLiterals,
          typename EscapeFunction>
std::optional<std::pair<std::string, const char*>>
ExportQueryExecutionTrees::idToStringAndType(const Index& index, Id id,
                                             const LocalVocab& localVocab,
                                             EscapeFunction&& escapeFunction) {
  using enum Datatype;
  auto datatype = id.getDatatype();
  if constexpr (onlyReturnLiterals) {
    if (!(datatype == VocabIndex || datatype == LocalVocabIndex)) {
      return std::nullopt;
    }
  }

  using LiteralOrIri = ad_utility::triple_component::LiteralOrIri;
  auto handleIriOrLiteral = [&escapeFunction](const LiteralOrIri& word)
      -> std::optional<std::pair<std::string, const char*>> {
    if constexpr (onlyReturnLiterals) {
      if (!word.isLiteral()) {
        return std::nullopt;
      }
    }
    if constexpr (removeQuotesAndAngleBrackets) {
      // TODO<joka921> Can we get rid of the string copying here?
      return std::pair{
          escapeFunction(std::string{asStringViewUnsafe(word.getContent())}),
          nullptr};
    }
    return std::pair{escapeFunction(word.toStringRepresentation()), nullptr};
  };
  switch (id.getDatatype()) {
    case Datatype::WordVocabIndex: {
      std::string_view entity = index.indexToString(id.getWordVocabIndex());
      return std::pair{escapeFunction(std::string{entity}), nullptr};
    }
    case VocabIndex:
    case LocalVocabIndex:
      return handleIriOrLiteral(
          getLiteralOrIriFromVocabIndex(index, id, localVocab));
    case TextRecordIndex:
      return std::pair{
          escapeFunction(index.getTextExcerpt(id.getTextRecordIndex())),
          nullptr};
    default:
      return idToStringAndTypeForEncodedValue(id);
  }
}
// _____________________________________________________________________________
template std::optional<std::pair<std::string, const char*>>
ExportQueryExecutionTrees::idToStringAndType<true, false, std::identity>(
    const Index& index, Id id, const LocalVocab& localVocab,
    std::identity&& escapeFunction);

// _____________________________________________________________________________
template std::optional<std::pair<std::string, const char*>>
ExportQueryExecutionTrees::idToStringAndType<true, true, std::identity>(
    const Index& index, Id id, const LocalVocab& localVocab,
    std::identity&& escapeFunction);

// This explicit instantiation is necessary because the `Variable` class
// currently still uses it.
// TODO<joka921> Refactor the CONSTRUCT export, then this is no longer
// needed
template std::optional<std::pair<std::string, const char*>>
ExportQueryExecutionTrees::idToStringAndType(const Index& index, Id id,
                                             const LocalVocab& localVocab,
                                             std::identity&& escapeFunction);

// _____________________________________________________________________________
static nlohmann::json stringAndTypeToBinding(std::string_view entitystr,
                                             const char* xsdType) {
  nlohmann::ordered_json b;
  if (xsdType) {
    b["value"] = entitystr;
    b["type"] = "literal";
    b["datatype"] = xsdType;
    return b;
  }

  // The string is an IRI or literal.
  if (entitystr.starts_with('<')) {
    // Strip the <> surrounding the iri.
    b["value"] = entitystr.substr(1, entitystr.size() - 2);
    // Even if they are technically IRIs, the format needs the type to be
    // "uri".
    b["type"] = "uri";
  } else if (entitystr.starts_with("_:")) {
    b["value"] = entitystr.substr(2);
    b["type"] = "bnode";
  } else {
    // TODO<joka921> This is probably not quite correct in the corner case
    // that there are datatype IRIs which contain quotes.
    size_t quotePos = entitystr.rfind('"');
    if (quotePos == std::string::npos) {
      // TEXT entries are currently not surrounded by quotes
      b["value"] = entitystr;
      b["type"] = "literal";
    } else {
      b["value"] = entitystr.substr(1, quotePos - 1);
      b["type"] = "literal";
      // Look for a language tag or type.
      if (quotePos < entitystr.size() - 1 && entitystr[quotePos + 1] == '@') {
        b["xml:lang"] = entitystr.substr(quotePos + 2);
      } else if (quotePos < entitystr.size() - 2 &&
                 entitystr[quotePos + 1] == '^') {
        AD_CONTRACT_CHECK(entitystr[quotePos + 2] == '^');
        std::string_view datatype{entitystr};
        // remove the <angledBrackets> around the datatype IRI
        AD_CONTRACT_CHECK(datatype.size() >= quotePos + 5);
        datatype.remove_prefix(quotePos + 4);
        datatype.remove_suffix(1);
        b["datatype"] = datatype;
      }
    }
  }
  return b;
}

// _____________________________________________________________________________
nlohmann::json ExportQueryExecutionTrees::selectQueryResultToSparqlJSON(
    const QueryExecutionTree& qet,
    const parsedQuery::SelectClause& selectClause,
    const LimitOffsetClause& limitAndOffset,
    std::shared_ptr<const Result> result,
    CancellationHandle cancellationHandle) {
  using nlohmann::json;

  AD_CORRECTNESS_CHECK(result != nullptr);
  LOG(DEBUG) << "Finished computing the query result in the ID space. "
                "Resolving strings in result...\n";

  // The `false` means "Don't include the question mark in the variable names".
  // TODO<joka921> Use a strong enum, and get rid of the comment.
  QueryExecutionTree::ColumnIndicesAndTypes columns =
      qet.selectedVariablesToColumnIndices(selectClause, false);

  std::erase(columns, std::nullopt);

  const IdTable& idTable = result->idTable();

  json resultJson;
  std::vector<std::string> selectedVars =
      selectClause.getSelectedVariablesAsStrings();
  // Strip the leading '?' from the variables, it is not part of the SPARQL JSON
  // output format.
  for (auto& var : selectedVars) {
    if (std::string_view{var}.starts_with('?')) {
      var = var.substr(1);
    }
  }
  resultJson["head"]["vars"] = selectedVars;

  json bindings = json::array();

  // TODO<joka921> add a warning to the result (Also for other formats).
  if (columns.empty()) {
    LOG(WARN) << "Exporting a SPARQL query where none of the selected "
                 "variables is bound in the query"
              << std::endl;
    resultJson["results"]["bindings"] = json::array();
    return resultJson;
  }

  for (size_t rowIndex : getRowIndices(limitAndOffset, *result)) {
    // TODO: ordered_json` entries are ordered alphabetically, but insertion
    // order would be preferable.
    nlohmann::ordered_json binding;
    for (const auto& column : columns) {
      const auto& currentId = idTable(rowIndex, column->columnIndex_);
      const auto& optionalValue = idToStringAndType(
          qet.getQec()->getIndex(), currentId, result->localVocab());
      if (!optionalValue.has_value()) {
        continue;
      }
      const auto& [stringValue, xsdType] = optionalValue.value();
      binding[column->variable_] = stringAndTypeToBinding(stringValue, xsdType);
    }
    bindings.emplace_back(std::move(binding));
    cancellationHandle->throwIfCancelled();
  }
  resultJson["results"]["bindings"] = std::move(bindings);
  return resultJson;
}

// _____________________________________________________________________________
nlohmann::json ExportQueryExecutionTrees::selectQueryResultBindingsToQLeverJSON(
    const QueryExecutionTree& qet,
    const parsedQuery::SelectClause& selectClause,
    const LimitOffsetClause& limitAndOffset,
    std::shared_ptr<const Result> result,
    CancellationHandle cancellationHandle) {
  AD_CORRECTNESS_CHECK(result != nullptr);
  LOG(DEBUG) << "Resolving strings for finished binary result...\n";
  QueryExecutionTree::ColumnIndicesAndTypes selectedColumnIndices =
      qet.selectedVariablesToColumnIndices(selectClause, true);

  return idTableToQLeverJSONArray(qet, limitAndOffset, selectedColumnIndices,
                                  std::move(result),
                                  std::move(cancellationHandle));
}

// _____________________________________________________________________________
cppcoro::generator<std::string>
ExportQueryExecutionTrees::selectQueryResultBindingsToQLeverJSONStream(
    const QueryExecutionTree& qet,
    const parsedQuery::SelectClause& selectClause,
    const LimitOffsetClause& limitAndOffset,
    std::shared_ptr<const Result> result,
    CancellationHandle cancellationHandle) {
  AD_CORRECTNESS_CHECK(result != nullptr);
  LOG(DEBUG) << "Resolving strings for finished binary result...\n";
  QueryExecutionTree::ColumnIndicesAndTypes selectedColumnIndices =
      qet.selectedVariablesToColumnIndices(selectClause, true);

  return idTableToQLeverJSONBindingsStream(
      qet, limitAndOffset, selectedColumnIndices, std::move(result),
      std::move(cancellationHandle));
}

// _____________________________________________________________________________
template <ad_utility::MediaType format>
ad_utility::streams::stream_generator
ExportQueryExecutionTrees::selectQueryResultToStream(
    const QueryExecutionTree& qet,
    const parsedQuery::SelectClause& selectClause,
    LimitOffsetClause limitAndOffset, CancellationHandle cancellationHandle) {
  static_assert(format == MediaType::octetStream || format == MediaType::csv ||
                format == MediaType::tsv || format == MediaType::turtle);

  // TODO<joka921> Use a proper error message, or check that we get a more
  // reasonable error from upstream.
  AD_CONTRACT_CHECK(format != MediaType::turtle);

  // This call triggers the possibly expensive computation of the query result
  // unless the result is already cached.
  std::shared_ptr<const Result> result = qet.getResult();
  result->logResultSize();
  LOG(DEBUG) << "Converting result IDs to their corresponding strings ..."
             << std::endl;
  auto selectedColumnIndices =
      qet.selectedVariablesToColumnIndices(selectClause, true);

  const auto& idTable = result->idTable();
  // special case : binary export of IdTable
  if constexpr (format == MediaType::octetStream) {
    for (size_t i : getRowIndices(limitAndOffset, *result)) {
      for (const auto& columnIndex : selectedColumnIndices) {
        if (columnIndex.has_value()) {
          co_yield std::string_view{reinterpret_cast<const char*>(&idTable(
                                        i, columnIndex.value().columnIndex_)),
                                    sizeof(Id)};
        }
      }
      cancellationHandle->throwIfCancelled();
    }
    co_return;
  }

  static constexpr char separator = format == MediaType::tsv ? '\t' : ',';
  // Print header line
  std::vector<std::string> variables =
      selectClause.getSelectedVariablesAsStrings();
  // In the CSV format, the variables don't include the question mark.
  if (format == MediaType::csv) {
    std::ranges::for_each(variables,
                          [](std::string& var) { var = var.substr(1); });
  }
  co_yield absl::StrJoin(variables, std::string_view{&separator, 1});
  co_yield '\n';

  constexpr auto& escapeFunction = format == MediaType::tsv
                                       ? RdfEscaping::escapeForTsv
                                       : RdfEscaping::escapeForCsv;
  for (size_t i : getRowIndices(limitAndOffset, *result)) {
    for (size_t j = 0; j < selectedColumnIndices.size(); ++j) {
      if (selectedColumnIndices[j].has_value()) {
        const auto& val = selectedColumnIndices[j].value();
        Id id = idTable(i, val.columnIndex_);
        auto optionalStringAndType =
            idToStringAndType<format == MediaType::csv>(
                qet.getQec()->getIndex(), id, result->localVocab(),
                escapeFunction);
        if (optionalStringAndType.has_value()) [[likely]] {
          co_yield optionalStringAndType.value().first;
        }
      }
      co_yield j + 1 < selectedColumnIndices.size() ? separator : '\n';
    }
    cancellationHandle->throwIfCancelled();
  }
  LOG(DEBUG) << "Done creating readable result.\n";
}

// Convert a single ID to an XML binding of the given `variable`.
static std::string idToXMLBinding(std::string_view variable, Id id,
                                  const auto& index, const auto& localVocab) {
  using namespace std::string_view_literals;
  using namespace std::string_literals;
  const auto& optionalValue =
      ExportQueryExecutionTrees::idToStringAndType(index, id, localVocab);
  if (!optionalValue.has_value()) {
    return ""s;
  }
  const auto& [stringValue, xsdType] = optionalValue.value();
  std::string result = absl::StrCat("\n    <binding name=\"", variable, "\">");
  auto append = [&](const auto&... values) {
    absl::StrAppend(&result, values...);
  };

  auto escape = [](std::string_view sv) {
    return RdfEscaping::escapeForXml(std::string{sv});
  };
  // Lambda that creates the inner content of the binding for the various
  // datatypes.
  auto strToBinding = [&result, &append, &escape](std::string_view entitystr) {
    // The string is an IRI or literal.
    if (entitystr.starts_with('<')) {
      // Strip the <> surrounding the iri.
      append("<uri>"sv, escape(entitystr.substr(1, entitystr.size() - 2)),
             "</uri>"sv);
    } else if (entitystr.starts_with("_:")) {
      append("<bnode>"sv, entitystr.substr(2), "</bnode>"sv);
    } else {
      size_t quotePos = entitystr.rfind('"');
      if (quotePos == std::string::npos) {
        absl::StrAppend(&result, "<literal>"sv, escape(entitystr),
                        "</literal>"sv);
      } else {
        std::string_view innerValue = entitystr.substr(1, quotePos - 1);
        // Look for a language tag or type.
        if (quotePos < entitystr.size() - 1 && entitystr[quotePos + 1] == '@') {
          std::string_view langtag = entitystr.substr(quotePos + 2);
          append("<literal xml:lang=\""sv, langtag, "\">"sv, escape(innerValue),
                 "</literal>"sv);
        } else if (quotePos < entitystr.size() - 2 &&
                   entitystr[quotePos + 1] == '^') {
          AD_CORRECTNESS_CHECK(entitystr[quotePos + 2] == '^');
          std::string_view datatype{entitystr};
          // remove the <angledBrackets> around the datatype IRI
          AD_CONTRACT_CHECK(datatype.size() >= quotePos + 5);
          datatype.remove_prefix(quotePos + 4);
          datatype.remove_suffix(1);
          append("<literal datatype=\""sv, escape(datatype), "\">"sv,
                 escape(innerValue), "</literal>"sv);
        } else {
          // A plain literal that contains neither a language tag nor a datatype
          append("<literal>"sv, escape(innerValue), "</literal>"sv);
        }
      }
    }
  };
  if (!xsdType) {
    // No xsdType, this means that `stringValue` is a plain string literal
    // or entity.
    strToBinding(stringValue);
  } else {
    append("<literal datatype=\""sv, xsdType, "\">"sv, stringValue,
           "</literal>");
  }
  append("</binding>");
  return result;
}

// _____________________________________________________________________________
template <>
ad_utility::streams::stream_generator ExportQueryExecutionTrees::
    selectQueryResultToStream<ad_utility::MediaType::sparqlXml>(
        const QueryExecutionTree& qet,
        const parsedQuery::SelectClause& selectClause,
        LimitOffsetClause limitAndOffset,
        CancellationHandle cancellationHandle) {
  using namespace std::string_view_literals;
  co_yield "<?xml version=\"1.0\"?>\n"
      "<sparql xmlns=\"http://www.w3.org/2005/sparql-results#\">";

  co_yield "\n<head>";
  std::vector<std::string> variables =
      selectClause.getSelectedVariablesAsStrings();
  // This call triggers the possibly expensive computation of the query result
  // unless the result is already cached.
  std::shared_ptr<const Result> result = qet.getResult();

  // In the XML format, the variables don't include the question mark.
  auto varsWithoutQuestionMark = std::views::transform(
      variables, [](std::string_view var) { return var.substr(1); });
  for (std::string_view var : varsWithoutQuestionMark) {
    co_yield absl::StrCat("\n  <variable name=\""sv, var, "\"/>"sv);
  }
  co_yield "\n</head>";

  co_yield "\n<results>";

  result->logResultSize();
  const auto& idTable = result->idTable();
  auto selectedColumnIndices =
      qet.selectedVariablesToColumnIndices(selectClause, false);
  // TODO<joka921> we could prefilter for the nonexisting variables.
  for (size_t i : getRowIndices(limitAndOffset, *result)) {
    co_yield "\n  <result>";
    for (size_t j = 0; j < selectedColumnIndices.size(); ++j) {
      if (selectedColumnIndices[j].has_value()) {
        const auto& val = selectedColumnIndices[j].value();
        Id id = idTable(i, val.columnIndex_);
        co_yield idToXMLBinding(val.variable_, id, qet.getQec()->getIndex(),
                                result->localVocab());
      }
    }
    co_yield "\n  </result>";
    cancellationHandle->throwIfCancelled();
  }
  co_yield "\n</results>";
  co_yield "\n</sparql>";
}

// _____________________________________________________________________________
template <>
ad_utility::streams::stream_generator ExportQueryExecutionTrees::
    selectQueryResultToStream<ad_utility::MediaType::sparqlJson>(
        const QueryExecutionTree& qet,
        const parsedQuery::SelectClause& selectClause,
        LimitOffsetClause limitAndOffset,
        CancellationHandle cancellationHandle) {
  // This call triggers the possibly expensive computation of the query result
  // unless the result is already cached.
  std::shared_ptr<const Result> result = qet.getResult();
  result->logResultSize();
  LOG(DEBUG) << "Converting result IDs to their corresponding strings ..."
             << std::endl;
  auto selectedColumnIndices =
      qet.selectedVariablesToColumnIndices(selectClause, false);

  const auto& idTable = result->idTable();

  auto vars = selectClause.getSelectedVariablesAsStrings();
  std::ranges::for_each(vars, [](std::string& var) { var = var.substr(1); });
  nlohmann::json jsonVars = vars;
  co_yield absl::StrCat(R"({"head":{"vars":)", jsonVars.dump(),
                        R"(},"results":{"bindings":[)");

  QueryExecutionTree::ColumnIndicesAndTypes columns =
      qet.selectedVariablesToColumnIndices(selectClause, false);
  std::erase(columns, std::nullopt);
  if (columns.empty()) {
    co_yield "]}}";
    co_return;
  }
  for (size_t i : getRowIndices(limitAndOffset, *result)) {
    nlohmann::ordered_json binding = {};
    for (const auto& column : columns) {
      const auto& currentId = idTable(i, column->columnIndex_);
      auto optionalStringAndType = idToStringAndType(
          qet.getQec()->getIndex(), currentId, result->localVocab());
      if (!optionalStringAndType) {
        continue;
      }
      const auto& [stringValue, xsdType] = optionalStringAndType.value();
      binding[column->variable_] = stringAndTypeToBinding(stringValue, xsdType);
    }
    co_yield absl::StrCat(i == 0 ? "" : ",", binding.dump());
    cancellationHandle->throwIfCancelled();
  }
  co_yield "]}}";
  co_return;
}

// _____________________________________________________________________________

// _____________________________________________________________________________
template <ad_utility::MediaType format>
ad_utility::streams::stream_generator
ExportQueryExecutionTrees::constructQueryResultToStream(
    const QueryExecutionTree& qet,
    const ad_utility::sparql_types::Triples& constructTriples,
    LimitOffsetClause limitAndOffset, std::shared_ptr<const Result> result,
    CancellationHandle cancellationHandle) {
  static_assert(format == MediaType::octetStream || format == MediaType::csv ||
                format == MediaType::tsv || format == MediaType::sparqlXml ||
                format == MediaType::sparqlJson);
  if constexpr (format == MediaType::octetStream) {
    AD_THROW("Binary export is not supported for CONSTRUCT queries");
  } else if constexpr (format == MediaType::sparqlXml) {
    AD_THROW("XML export is currently not supported for CONSTRUCT queries");
  }
  result->logResultSize();
  constexpr auto& escapeFunction = format == MediaType::tsv
                                       ? RdfEscaping::escapeForTsv
                                       : RdfEscaping::escapeForCsv;
  constexpr char sep = format == MediaType::tsv ? '\t' : ',';
  auto generator =
      constructQueryResultToTriples(qet, constructTriples, limitAndOffset,
                                    result, std::move(cancellationHandle));
  for (auto& triple : generator) {
    co_yield escapeFunction(std::move(triple.subject_));
    co_yield sep;
    co_yield escapeFunction(std::move(triple.predicate_));
    co_yield sep;
    co_yield escapeFunction(std::move(triple.object_));
    co_yield "\n";
  }
}

// _____________________________________________________________________________
nlohmann::json ExportQueryExecutionTrees::computeQueryResultAsQLeverJSON(
    const ParsedQuery& query, const QueryExecutionTree& qet,
    const ad_utility::Timer& requestTimer,
    CancellationHandle cancellationHandle) {
  std::shared_ptr<const Result> result = qet.getResult();
  result->logResultSize();
  auto timeResultComputation = requestTimer.msecs();

  size_t resultSize = result->idTable().size();

  nlohmann::json j;

  j["query"] = query._originalString;
  j["status"] = "OK";
  j["warnings"] = qet.collectWarnings();
  if (query.hasSelectClause()) {
    j["selected"] = query.selectClause().getSelectedVariablesAsStrings();
  } else {
    j["selected"] =
        std::vector<std::string>{"?subject", "?predicate", "?object"};
  }

  j["runtimeInformation"]["meta"] = nlohmann::ordered_json(
      qet.getRootOperation()->getRuntimeInfoWholeQuery());
  RuntimeInformation runtimeInformation = qet.getRootOperation()->runtimeInfo();
  runtimeInformation.addLimitOffsetRow(
      query._limitOffset, std::chrono::milliseconds::zero(), false);
  j["runtimeInformation"]["query_execution_tree"] =
      nlohmann::ordered_json(runtimeInformation);

  {
    j["res"] =
        query.hasSelectClause()
            ? selectQueryResultBindingsToQLeverJSON(
                  qet, query.selectClause(), query._limitOffset,
                  std::move(result), std::move(cancellationHandle))
            : constructQueryResultBindingsToQLeverJSON(
                  qet, query.constructClause().triples_, query._limitOffset,
                  std::move(result), std::move(cancellationHandle));
  }
  j["resultsize"] = query.hasSelectClause() ? resultSize : j["res"].size();
  j["time"]["total"] = std::to_string(requestTimer.msecs().count()) + "ms";
  j["time"]["computeResult"] =
      std::to_string(timeResultComputation.count()) + "ms";

  return j;
}

// _____________________________________________________________________________
cppcoro::generator<std::string>
ExportQueryExecutionTrees::computeResultAsStream(
    const ParsedQuery& parsedQuery, const QueryExecutionTree& qet,
    ad_utility::MediaType mediaType, CancellationHandle cancellationHandle) {
  auto compute = [&]<MediaType format> {
    auto limitAndOffset = parsedQuery._limitOffset;
    return parsedQuery.hasSelectClause()
               ? ExportQueryExecutionTrees::selectQueryResultToStream<format>(
                     qet, parsedQuery.selectClause(), limitAndOffset,
                     std::move(cancellationHandle))
               : ExportQueryExecutionTrees::constructQueryResultToStream<
                     format>(qet, parsedQuery.constructClause().triples_,
                             limitAndOffset, qet.getResult(),
                             std::move(cancellationHandle));
  };

  using enum MediaType;
  auto inner =
      ad_utility::ConstexprSwitch<csv, tsv, octetStream, turtle, sparqlXml,
                                  sparqlJson>(compute, mediaType);
  try {
    for (auto& block : inner) {
      co_yield std::move(block);
    }
  } catch (ad_utility::CancellationException& e) {
    e.setOperation("Stream query export");
    throw;
  }
}

// _____________________________________________________________________________
nlohmann::json ExportQueryExecutionTrees::computeSelectQueryResultAsSparqlJSON(
    const ParsedQuery& query, const QueryExecutionTree& qet,
    CancellationHandle cancellationHandle) {
  if (!query.hasSelectClause()) {
    AD_THROW(
        "SPARQL-compliant JSON format is only supported for SELECT queries");
  }
  std::shared_ptr<const Result> result = qet.getResult();
  result->logResultSize();
  return selectQueryResultToSparqlJSON(qet, query.selectClause(),
                                       query._limitOffset, std::move(result),
                                       std::move(cancellationHandle));
}

// _____________________________________________________________________________
nlohmann::json ExportQueryExecutionTrees::computeResultAsJSON(
    const ParsedQuery& parsedQuery, const QueryExecutionTree& qet,
    const ad_utility::Timer& requestTimer, ad_utility::MediaType mediaType,
    CancellationHandle cancellationHandle) {
  try {
    switch (mediaType) {
      case ad_utility::MediaType::qleverJson:
        return computeQueryResultAsQLeverJSON(parsedQuery, qet, requestTimer,
                                              std::move(cancellationHandle));
      case ad_utility::MediaType::sparqlJson:
        return computeSelectQueryResultAsSparqlJSON(
            parsedQuery, qet, std::move(cancellationHandle));
      default:
        AD_FAIL();
    }
  } catch (ad_utility::CancellationException& e) {
    e.setOperation("Query export");
    throw;
  }
}

// _____________________________________________________________________________
cppcoro::generator<std::string>
ExportQueryExecutionTrees::computeResultAsQLeverJSONStream(
    const ParsedQuery& query, const QueryExecutionTree& qet,
    const ad_utility::Timer& requestTimer,
    CancellationHandle cancellationHandle) {
  std::shared_ptr<const Result> result = qet.getResult();
  result->logResultSize();
  auto timeResultComputation = requestTimer.msecs();

  nlohmann::json jsonPrefix;

  jsonPrefix["query"] = query._originalString;
  jsonPrefix["status"] = "OK";
  jsonPrefix["warnings"] = qet.collectWarnings();
  if (query.hasSelectClause()) {
    jsonPrefix["selected"] =
        query.selectClause().getSelectedVariablesAsStrings();
  } else {
    jsonPrefix["selected"] =
        std::vector<std::string>{"?subject", "?predicate", "?object"};
  }

  jsonPrefix["runtimeInformation"]["meta"] = nlohmann::ordered_json(
      qet.getRootOperation()->getRuntimeInfoWholeQuery());
  RuntimeInformation runtimeInformation = qet.getRootOperation()->runtimeInfo();
  runtimeInformation.addLimitOffsetRow(
      query._limitOffset, std::chrono::milliseconds::zero(), false);
  jsonPrefix["runtimeInformation"]["query_execution_tree"] =
      nlohmann::ordered_json(runtimeInformation);

  std::string prefixStr = jsonPrefix.dump();
  co_yield absl::StrCat(prefixStr.substr(0, prefixStr.size() - 1),
                        R"(,"res":[)");

  auto bindings =
      query.hasSelectClause()
          ? selectQueryResultBindingsToQLeverJSONStream(
                qet, query.selectClause(), query._limitOffset,
                std::move(result), std::move(cancellationHandle))
          : constructQueryResultBindingsToQLeverJSONStream(
                qet, query.constructClause().triples_, query._limitOffset,
                std::move(result), std::move(cancellationHandle));

  size_t resultSize = 0;
  for (std::string& b : bindings) {
    if (resultSize > 0) [[likely]] {
      co_yield ",";
    }
    co_yield b;
    ++resultSize;
  }

  nlohmann::json jsonSuffix;
  jsonSuffix["resultsize"] = resultSize;
  jsonSuffix["time"]["total"] =
      absl::StrCat(requestTimer.msecs().count(), "ms");
  jsonSuffix["time"]["computeResult"] =
      absl::StrCat(timeResultComputation.count(), "ms");

  co_yield absl::StrCat("],", jsonSuffix.dump().substr(1));
}
