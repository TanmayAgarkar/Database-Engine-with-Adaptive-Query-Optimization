// SQL tokenizer, AST and recursive-descent parser for the supported subset.
// Identifiers and keywords are case-insensitive (identifiers are stored lowercase).
#pragma once
#include <algorithm>
#include <cctype>

#include "types.hpp"

namespace adb {

enum class Op { EQ, NE, LT, LE, GT, GE };
inline const char* opText(Op o) {
    switch (o) {
        case Op::EQ: return "=";
        case Op::NE: return "!=";
        case Op::LT: return "<";
        case Op::LE: return "<=";
        case Op::GT: return ">";
        case Op::GE: return ">=";
    }
    return "?";
}

struct Cond {
    std::string column;
    Op op;
    Value value;
};

struct Statement {
    enum Kind { CreateTable, Insert, Select, Update, Delete, CreateIndex, DropIndex, ShowTables,
                Describe, ShowRecs, ApplyRecs, ShowMetrics, Generate, SetCache } kind = Select;
    bool explain = false;
    bool flag = false;  // SET CACHE ON/OFF
    std::string table, index_name, column;
    std::vector<Column> col_defs;
    std::vector<Row> rows;
    std::vector<std::string> projection;  // empty == *
    std::vector<Cond> where;              // conjunction (AND)
    std::vector<std::pair<std::string, Value>> assignments;
    int64_t count = 0;

    // Normalised text of a SELECT; parameterize=true replaces literals by '?'
    // (workload fingerprint), otherwise it is the exact result-cache key.
    std::string canonical(bool parameterize) const {
        std::ostringstream o;
        o << "select ";
        if (projection.empty()) o << '*';
        for (size_t i = 0; i < projection.size(); ++i) o << (i ? "," : "") << projection[i];
        o << " from " << table;
        for (size_t i = 0; i < where.size(); ++i)
            o << (i ? " and " : " where ") << where[i].column << ' ' << opText(where[i].op) << ' '
              << (parameterize ? std::string("?") : literal(where[i].value));
        return o.str();
    }
};

// ---- tokenizer ---------------------------------------------------------------
enum class Tok { Ident, Int, Float, Str, Sym, End };
struct Token { Tok kind; std::string text; };

inline std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> t;
    size_t i = 0, n = s.size();
    auto uc = [](char c) { return static_cast<unsigned char>(c); };
    while (i < n) {
        char c = s[i];
        if (std::isspace(uc(c))) { ++i; continue; }
        if (std::isalpha(uc(c)) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(uc(s[j])) || s[j] == '_')) ++j;
            std::string w = s.substr(i, j - i);
            std::transform(w.begin(), w.end(), w.begin(), [](unsigned char ch) { return std::tolower(ch); });
            t.push_back({Tok::Ident, w});
            i = j;
        } else if (std::isdigit(uc(c))) {
            size_t j = i;
            bool fl = false;
            while (j < n && std::isdigit(uc(s[j]))) ++j;
            if (j + 1 < n && s[j] == '.' && std::isdigit(uc(s[j + 1]))) {
                fl = true; ++j;
                while (j < n && std::isdigit(uc(s[j]))) ++j;
            }
            t.push_back({fl ? Tok::Float : Tok::Int, s.substr(i, j - i)});
            i = j;
        } else if (c == '\'') {
            std::string v;
            size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                if (s[j] == '\'') {
                    if (j + 1 < n && s[j + 1] == '\'') { v += '\''; j += 2; continue; }
                    closed = true; ++j; break;
                }
                v += s[j++];
            }
            if (!closed) throw ParseError("unterminated string literal");
            t.push_back({Tok::Str, v});
            i = j;
        } else if ((c == '<' || c == '>' || c == '!') && i + 1 < n && s[i + 1] == '=') {
            t.push_back({Tok::Sym, s.substr(i, 2)}); i += 2;
        } else if (c == '<' && i + 1 < n && s[i + 1] == '>') {
            t.push_back({Tok::Sym, "!="}); i += 2;
        } else if (std::string("(),;*=<>-").find(c) != std::string::npos) {
            t.push_back({Tok::Sym, std::string(1, c)}); ++i;
        } else {
            throw ParseError(std::string("unexpected character '") + c + "'");
        }
    }
    t.push_back({Tok::End, ""});
    return t;
}

// ---- parser -------------------------------------------------------------------
class Parser {
public:
    explicit Parser(const std::string& sql) : t_(tokenize(sql)) {}

    Statement parse() {
        Statement st;
        if (kw("explain")) { ++p_; st.explain = true; if (!kw("select")) fail("EXPLAIN supports SELECT only"); }
        if (kw("create")) {
            ++p_;
            if (kw("table")) { ++p_; parseCreateTable(st); }
            else if (kw("index")) { ++p_; parseCreateIndex(st); }
            else fail("expected TABLE or INDEX after CREATE");
        } else if (kw("insert")) { ++p_; parseInsert(st);
        } else if (kw("select")) { ++p_; parseSelect(st);
        } else if (kw("update")) { ++p_; parseUpdate(st);
        } else if (kw("delete")) { ++p_; parseDelete(st);
        } else if (kw("drop")) {
            ++p_; expectKw("index"); st.kind = Statement::DropIndex; st.index_name = ident();
        } else if (kw("show")) {
            ++p_;
            if (kw("tables")) { ++p_; st.kind = Statement::ShowTables; }
            else if (kw("metrics")) { ++p_; st.kind = Statement::ShowMetrics; }
            else if (kw("index")) { ++p_; expectKw("recommendations"); st.kind = Statement::ShowRecs; }
            else fail("expected TABLES, METRICS or INDEX RECOMMENDATIONS after SHOW");
        } else if (kw("describe")) { ++p_; st.kind = Statement::Describe; st.table = ident();
        } else if (kw("apply")) {
            ++p_; expectKw("index"); expectKw("recommendations"); st.kind = Statement::ApplyRecs;
        } else if (kw("generate")) {
            ++p_; st.kind = Statement::Generate; st.table = ident();
            if (t_[p_].kind != Tok::Int) fail("GENERATE expects a row count");
            st.count = std::stoll(t_[p_++].text);
        } else if (kw("set")) {
            ++p_; expectKw("cache"); st.kind = Statement::SetCache;
            if (kw("on")) st.flag = true; else if (kw("off")) st.flag = false; else fail("expected ON or OFF");
            ++p_;
        } else {
            fail("unsupported or empty statement");
        }
        if (sym(";")) ++p_;
        if (t_[p_].kind != Tok::End) fail("unexpected trailing input near '" + t_[p_].text + "'");
        return st;
    }

private:
    [[noreturn]] static void fail(const std::string& m) { throw ParseError(m); }
    bool kw(const char* w) const { return t_[p_].kind == Tok::Ident && t_[p_].text == w; }
    bool sym(const char* s) const { return t_[p_].kind == Tok::Sym && t_[p_].text == s; }
    void expectKw(const char* w) { if (!kw(w)) fail(std::string("expected ") + w); ++p_; }
    void expectSym(const char* s) { if (!sym(s)) fail(std::string("expected '") + s + "'"); ++p_; }
    std::string ident() {
        if (t_[p_].kind != Tok::Ident) fail("expected identifier");
        return t_[p_++].text;
    }

    Value literalValue() {
        bool neg = false;
        if (sym("-")) { neg = true; ++p_; }
        const Token& k = t_[p_];
        try {
            if (k.kind == Tok::Int) { ++p_; int64_t v = std::stoll(k.text); return Value(neg ? -v : v); }
            if (k.kind == Tok::Float) { ++p_; double v = std::stod(k.text); return Value(neg ? -v : v); }
        } catch (const std::out_of_range&) { fail("numeric literal out of range"); }
        if (neg) fail("expected number after '-'");
        if (k.kind == Tok::Str) { ++p_; return Value(std::string(k.text)); }
        if (kw("true")) { ++p_; return Value(true); }
        if (kw("false")) { ++p_; return Value(false); }
        if (kw("null")) { ++p_; return Value(std::monostate{}); }
        fail("expected a literal value");
    }

    Type typeName() {
        std::string w = ident();
        if (w == "int" || w == "integer") return Type::INT;
        if (w == "double" || w == "float" || w == "real") return Type::DOUBLE;
        if (w == "text" || w == "string") return Type::TEXT;
        if (w == "bool" || w == "boolean") return Type::BOOL;
        fail("unknown column type: " + w);
    }

    void parseCreateTable(Statement& st) {
        st.kind = Statement::CreateTable;
        st.table = ident();
        expectSym("(");
        do { std::string n = ident(); st.col_defs.push_back({n, typeName()}); } while (sym(",") && (++p_, true));
        expectSym(")");
    }
    void parseCreateIndex(Statement& st) {
        st.kind = Statement::CreateIndex;
        st.index_name = ident();
        expectKw("on");
        st.table = ident();
        expectSym("(");
        st.column = ident();
        expectSym(")");
    }
    void parseInsert(Statement& st) {
        st.kind = Statement::Insert;
        expectKw("into");
        st.table = ident();
        expectKw("values");
        do {
            expectSym("(");
            Row r;
            do { r.push_back(literalValue()); } while (sym(",") && (++p_, true));
            expectSym(")");
            st.rows.push_back(std::move(r));
        } while (sym(",") && (++p_, true));
    }
    void parseWhere(Statement& st) {
        if (!kw("where")) return;
        ++p_;
        do {
            Cond c;
            c.column = ident();
            if (sym("=")) c.op = Op::EQ; else if (sym("!=")) c.op = Op::NE; else if (sym("<")) c.op = Op::LT;
            else if (sym("<=")) c.op = Op::LE; else if (sym(">")) c.op = Op::GT; else if (sym(">=")) c.op = Op::GE;
            else fail("expected comparison operator");
            ++p_;
            c.value = literalValue();
            st.where.push_back(std::move(c));
        } while (kw("and") && (++p_, true));
        if (kw("or")) fail("OR is not supported (only AND)");
    }
    void parseSelect(Statement& st) {
        st.kind = Statement::Select;
        if (sym("*")) ++p_;
        else do { st.projection.push_back(ident()); } while (sym(",") && (++p_, true));
        expectKw("from");
        st.table = ident();
        parseWhere(st);
    }
    void parseUpdate(Statement& st) {
        st.kind = Statement::Update;
        st.table = ident();
        expectKw("set");
        do {
            std::string c = ident();
            expectSym("=");
            st.assignments.emplace_back(c, literalValue());
        } while (sym(",") && (++p_, true));
        parseWhere(st);
    }
    void parseDelete(Statement& st) {
        st.kind = Statement::Delete;
        expectKw("from");
        st.table = ident();
        parseWhere(st);
    }

    std::vector<Token> t_;
    size_t p_ = 0;
};

inline Statement parseStatement(const std::string& sql) { return Parser(sql).parse(); }

}  // namespace adb

