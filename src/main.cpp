#include <filesystem>
#include <iostream>

#include "database.hpp"

using namespace adb;

static void printResult(const ExecResult& r) {
    if (r.is_select) {
        const auto& cols = r.data.columns;
        std::vector<size_t> w(cols.size());
        for (size_t i = 0; i < cols.size(); ++i) w[i] = cols[i].size();
        const size_t show = std::min<size_t>(r.data.rows.size(), 50);
        for (size_t k = 0; k < show; ++k)
            for (size_t i = 0; i < cols.size(); ++i) w[i] = std::max(w[i], formatValue(r.data.rows[k][i]).size());
        auto line = [&] { for (size_t i = 0; i < cols.size(); ++i) std::cout << '+' << std::string(w[i] + 2, '-'); std::cout << "+\n"; };
        line();
        for (size_t i = 0; i < cols.size(); ++i) std::cout << "| " << std::left << std::setw(static_cast<int>(w[i])) << cols[i] << ' ';
        std::cout << "|\n";
        line();
        for (size_t k = 0; k < show; ++k) {
            for (size_t i = 0; i < cols.size(); ++i) std::cout << "| " << std::left << std::setw(static_cast<int>(w[i])) << formatValue(r.data.rows[k][i]) << ' ';
            std::cout << "|\n";
        }
        line();
        std::cout << r.data.rows.size() << " row(s)" << (r.data.rows.size() > show ? " (first 50 shown)" : "") << "\n";
        const QueryStats& s = r.stats;
        if (s.access_method != "n/a") {
            std::cout << "[" << s.access_method << (s.index_used.empty() ? "" : " using " + s.index_used) << " | examined " << s.rows_examined
                      << " | returned " << s.rows_returned << " | " << std::fixed << std::setprecision(3) << s.exec_ms << " ms | page requests "
                      << s.logical_reads << " (disk reads " << s.physical_reads << ")]\n";
            std::cout.unsetf(std::ios::floatfield);
        }
    }
    if (!r.message.empty()) std::cout << r.message << (r.message.back() == '\n' ? "" : "\n");
}

// Split buffered input into complete statements (';' outside quotes); leftover stays in buf.
static std::vector<std::string> takeStatements(std::string& buf) {
    std::vector<std::string> out;
    bool inq = false;
    size_t start = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] == '\'') inq = !inq;
        else if (buf[i] == ';' && !inq) { out.push_back(buf.substr(start, i - start + 1)); start = i + 1; }
    }
    buf.erase(0, start);
    return out;
}

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "data/adaptive.db";
    try {
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        Database db(path);
        bool tty = true;
        std::cout << "Adaptive Mini DBMS v1.0\nDatabase opened: " << path << "\nType SQL ending with ';'  (.quit to exit)\n\n";
        std::string buf, line;
        while (true) {
            if (tty) std::cout << (buf.empty() ? "db> " : "  > ") << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (buf.empty() && (line == ".quit" || line == ".exit" || line == "quit" || line == "exit")) break;
            buf += line + "\n";
            for (const auto& stmt : takeStatements(buf)) {
                try { printResult(db.execute(stmt)); }
                catch (const DbError& e) { std::cout << "Error: " << e.what() << "\n"; }
            }
        }
        db.flush();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

