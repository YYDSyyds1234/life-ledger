/* =====================================================================
   生活記帳簿（Life Ledger）— Dev-C++ 可直接編譯執行的命令列版
   ---------------------------------------------------------------------
   用途：把「生活記帳小助手」網站的核心功能做成可在 Windows 主控台
         執行的單一 C++ 程式，適合交作業、也適合直接拿來記帳。

   功能：多帳本、記一筆、清單（分頁）、編輯／刪除、搜尋、月統計、
         分類統計、總預算＋分類預算警戒、匯出 CSV、備份、自動存檔。

   Dev-C++ 使用方式：
     1. 開 Dev-C++ → 檔案 → 開啟 → 選這個 .cpp
     2. 直接按 F11（編譯並執行），不需要額外設定
     3. 若主控台中文變亂碼：Dev-C++ 的「工具 → 編譯器選項」加上
        -fexec-charset=big5（或直接用 Windows 10/11 的終端機執行）

   資料存放：程式所在資料夾的 ledger_data.txt（純文字，可自行備份）
   ===================================================================== */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#include <windows.h>     // 只用來把主控台切成 UTF-8，讓中文正常顯示
#endif

using namespace std;

/* ===================== 一、常數與資料結構 ===================== */

static const int CAT_N = 9;                                   // 類別數量
static const char *CAT_NAME[CAT_N] = {                        // 固定九個類別（與網站一致）
    "餐飲", "交通", "日用品", "娛樂", "醫療", "學費", "房租", "通訊", "其他"
};
static const char *BOOK_TYPE[5] = {                            // 帳本類型（僅作標記用）
    "日常", "旅遊", "專案", "公司", "其他"
};
static const char *DATA_FILE = "ledger_data.txt";              // 資料檔名
static const int PAGE_SIZE = 15;                               // 清單每頁 15 筆

// 一筆消費紀錄
struct Rec {
    string date;      // 日期，格式 YYYY-MM-DD
    int    cat;       // 類別索引 0..CAT_N-1
    double amount;    // 金額（新台幣）
    string note;      // 備註，可空
};

// 一本帳
struct Book {
    string       name;                       // 帳本名稱
    int          type;                       // 帳本類型索引
    vector<Rec>  recs;                       // 這本帳的所有紀錄
    double       budgetTotal;                // 每月總預算，0 表示未設定
    double       budgetCat[CAT_N];           // 各分類預算，0 表示未設定
    Book() : type(0), budgetTotal(0) {
        for (int i = 0; i < CAT_N; ++i) budgetCat[i] = 0;
    }
};

/* ===================== 二、全域狀態 ===================== */

static vector<Book> g_books;        // 所有帳本
static int  g_cur = 0;              // 目前使用的帳本索引
static bool g_alertTotal80 = false; // 總預算是否已提示 80%
static bool g_alertTotal100 = false;// 總預算是否已提示 100%
static bool g_alertCat80[CAT_N];    // 各分類是否已提示 80%
static bool g_alertCat100[CAT_N];   // 各分類是否已提示 100%

/* ===================== 三、小工具函式 ===================== */

// 去掉字串前後空白（使用者按空白鍵很容易多打）
static string trim(const string &s) {
    int b = 0, e = (int)s.size() - 1;
    while (b <= e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e >= b && (s[e] == ' ' || s[e] == '\t' || s[e] == '\r')) --e;
    string r;
    for (int i = b; i <= e; ++i) r += s[i];
    return r;
}

// 把 double 轉成「1234.56」這種字串（金額一律兩位小數）
static string money(double v) {
    ostringstream os;
    os << fixed << setprecision(2) << v;
    return os.str();
}

// 把整數轉字串（避免用 to_string，確保舊版 Dev-C++ 也能編）
static string itos(int v) {
    ostringstream os;
    os << v;
    return os.str();
}

// 計算字串在「終端機上」顯示的寬度：UTF-8 中文佔 3 bytes 但只算 2 格
static int dispWidth(const string &s) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) ++w;                 // ASCII 佔 1 格
        else if ((c >> 5) == 0x6) { w += 1; i += 1; }   // 2 bytes 字元
        else if ((c >> 4) == 0xE) { w += 2; i += 2; }   // 3 bytes（中文）佔 2 格
        else if ((c >> 3) == 0x1E) { w += 2; i += 3; }  // 4 bytes（emoji 等）
    }
    return w;
}

// 把字串補空白到指定顯示寬度，讓表格看起來整齊
static string padRight(const string &s, int width) {
    string r = s;
    int w = dispWidth(s);
    for (int i = w; i < width; ++i) r += ' ';
    return r;
}

// 把字串截斷到指定顯示寬度（備註太長時用）
static string cutTo(const string &s, int width) {
    string r;
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        int cw = 1, extra = 0;
        if (c < 0x80) { cw = 1; extra = 0; }
        else if ((c >> 5) == 0x6) { cw = 1; extra = 1; }
        else if ((c >> 4) == 0xE) { cw = 2; extra = 2; }
        else { cw = 2; extra = 3; }
        if (w + cw > width) { r += "…"; break; }
        for (int k = 0; k <= extra && i + k < s.size(); ++k) r += s[i + k];
        i += extra;
        w += cw;
    }
    return r;
}

// 印出一條分隔線
static void hr(char c = '-', int n = 62) {
    for (int i = 0; i < n; ++i) cout << c;
    cout << "\n";
}

// 取得「今天」字串（YYYY-MM-DD）
static string today() {
    time_t t = time(0);
    struct tm *lt = localtime(&t);
    char buf[16];
    sprintf(buf, "%04d-%02d-%02d", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    return string(buf);
}

// 判斷日期字串是否合法（YYYY-MM-DD，且年月日合理）
static bool isDate(const string &s) {
    if (s.size() != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int y = atoi(s.substr(0, 4).c_str());
    int m = atoi(s.substr(5, 2).c_str());
    int d = atoi(s.substr(8, 2).c_str());
    if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    return true;
}

// 安全讀一整行；遇到 EOF（例如管線輸入結束）回傳 false，避免無限迴圈
static bool readLine(const string &prompt, string &out) {
    cout << prompt;
    cout.flush();
    if (!getline(cin, out)) return false;
    if (!out.empty() && out[out.size() - 1] == '\r') out.erase(out.size() - 1);
    out = trim(out);
    return true;
}

// 讀一個整數並限制範圍；輸入非數字會重問
static bool readInt(const string &prompt, int lo, int hi, int &out) {
    string s;
    while (true) {
        if (!readLine(prompt, s)) return false;
        if (s.empty()) continue;
        bool ok = true;
        for (size_t i = 0; i < s.size(); ++i)
            if (!(isdigit((unsigned char)s[i]) || (i == 0 && s[i] == '-'))) { ok = false; break; }
        if (!ok) { cout << "  ⚠ 請輸入數字。\n"; continue; }
        int v = atoi(s.c_str());
        if (v < lo || v > hi) { cout << "  ⚠ 請輸入 " << lo << " 到 " << hi << " 之間的數字。\n"; continue; }
        out = v;
        return true;
    }
}

// 讀一個金額（大於 0）
static bool readAmount(const string &prompt, double &out) {
    string s;
    while (true) {
        if (!readLine(prompt, s)) return false;
        if (s.empty()) continue;
        bool ok = true; int dot = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '.') { ++dot; if (dot > 1) ok = false; }
            else if (!isdigit((unsigned char)s[i])) ok = false;
        }
        double v = atof(s.c_str());
        if (!ok || v <= 0) { cout << "  ⚠ 金額要填大於 0 的數字。\n"; continue; }
        out = v;
        return true;
    }
}

// 讀一個金額（允許 0，用在「取消預算」這種情況）
static bool readAmountAllowZero(const string &prompt, double &out) {
    string s;
    while (true) {
        if (!readLine(prompt, s)) return false;
        if (s.empty()) continue;
        bool ok = true; int dot = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '.') { ++dot; if (dot > 1) ok = false; }
            else if (!isdigit((unsigned char)s[i])) ok = false;
        }
        double v = atof(s.c_str());
        if (!ok || v < 0) { cout << "  ⚠ 請輸入 0 或大於 0 的數字。\n"; continue; }
        out = v;
        return true;
    }
}

// 在畫面上停一下，讓使用者看完再回主選單
static void pause() {
    string dummy;
    cout << "\n按 Enter 回主選單…";
    cout.flush();
    getline(cin, dummy);
}

// 目前帳本的參考（用參考避免每次複製整本帳）
static Book &curBook() { return g_books[g_cur]; }

// 清空「已提示」旗標，避免預算警告重複轟炸
static void resetAlerts() {
    g_alertTotal80 = g_alertTotal100 = false;
    for (int i = 0; i < CAT_N; ++i) { g_alertCat80[i] = false; g_alertCat100[i] = false; }
}

/* ===================== 四、存檔與讀檔 ===================== */

// 把備註裡的 | 與換行換掉，避免破壞檔案格式
static string safeField(const string &s) {
    string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '|') r += '/';
        else if (s[i] == '\n' || s[i] == '\r') r += ' ';
        else r += s[i];
    }
    return r;
}

// 存檔：先寫 BOOK 行，再寫該帳本的 REC 行
static void saveData() {
    ofstream f(DATA_FILE);                    // 覆寫整個檔案
    if (!f) { cout << "  ⚠ 無法寫入 " << DATA_FILE << "\n"; return; }
    for (size_t b = 0; b < g_books.size(); ++b) {
        const Book &bk = g_books[b];
        f << "BOOK|" << safeField(bk.name) << "|" << bk.type << "|" << money(bk.budgetTotal);
        for (int i = 0; i < CAT_N; ++i) f << "|" << money(bk.budgetCat[i]);
        f << "\n";
        for (size_t r = 0; r < bk.recs.size(); ++r) {
            const Rec &x = bk.recs[r];
            f << "REC|" << x.date << "|" << x.cat << "|" << money(x.amount) << "|"
              << safeField(x.note) << "\n";
        }
    }
    f.close();
}

// 以 | 切字串
static vector<string> split(const string &s, char sep) {
    vector<string> v;
    string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == sep) { v.push_back(cur); cur = ""; }
        else cur += s[i];
    }
    v.push_back(cur);
    return v;
}

// 讀檔；檔案不存在就建立一本預設帳，讓程式第一次跑也不會壞
static void loadData() {
    g_books.clear();
    ifstream f(DATA_FILE);
    if (!f) {
        Book bk; bk.name = "我的日常帳"; bk.type = 0;
        g_books.push_back(bk);
        g_cur = 0;
        return;
    }
    string line;
    while (getline(f, line)) {                   // 讀到檔尾為止
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.empty()) continue;
        vector<string> p = split(line, '|');
        if (p[0] == "BOOK" && p.size() >= 4 + CAT_N) {
            Book bk;
            bk.name = p[1];
            bk.type = atoi(p[2].c_str());
            bk.budgetTotal = atof(p[3].c_str());
            for (int i = 0; i < CAT_N; ++i) bk.budgetCat[i] = atof(p[4 + i].c_str());
            g_books.push_back(bk);
        } else if (p[0] == "REC" && p.size() >= 5 && !g_books.empty()) {
            Rec x;
            x.date = p[1];
            x.cat = atoi(p[2].c_str());
            x.amount = atof(p[3].c_str());
            x.note = p[4];
            if (x.cat < 0 || x.cat >= CAT_N) x.cat = CAT_N - 1;
            g_books.back().recs.push_back(x);
        }
    }
    f.close();
    if (g_books.empty()) {                       // 檔案存在但沒資料時的保險
        Book bk; bk.name = "我的日常帳"; bk.type = 0;
        g_books.push_back(bk);
    }
    g_cur = 0;
}

/* ===================== 五、篩選與統計 ===================== */

// 收集符合條件的紀錄索引；cat = -1 表示全部，month 空字串表示全部月份
static vector<int> collectIdx(int cat, const string &month, const string &q) {
    vector<Rec> &recs = curBook().recs;
    vector<int> out;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (cat >= 0 && recs[i].cat != cat) continue;
        if (!month.empty() && recs[i].date.substr(0, 7) != month) continue;
        if (!q.empty()) {
            string hay = string(CAT_NAME[recs[i].cat]) + " " + recs[i].note;
            if (hay.find(q) == string::npos) continue;
        }
        out.push_back((int)i);
    }
    // 依日期新到舊排序；同日期則後輸入的排前面
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1; j < out.size(); ++j) {
            bool swapNeeded = false;
            if (recs[out[j]].date > recs[out[i]].date) swapNeeded = true;
            else if (recs[out[j]].date == recs[out[i]].date && out[j] > out[i]) swapNeeded = true;
            if (swapNeeded) { int t = out[i]; out[i] = out[j]; out[j] = t; }
        }
    return out;
}

// 印出紀錄清單（分頁），回傳實際顯示的索引（供編輯／刪除使用編號對應）
static vector<int> printList(const vector<int> &idx) {
    if (idx.empty()) {
        cout << "  （目前沒有符合條件的紀錄）\n";
        return idx;
    }
    int start = 0;
    while (true) {
        hr('=');
        cout << "  " << padRight("編號", 6) << padRight("日期", 12) << padRight("類別", 10)
             << padRight("金額", 12) << "備註\n";
        hr();
        int end = start + PAGE_SIZE;
        if (end > (int)idx.size()) end = (int)idx.size();
        for (int k = start; k < end; ++k) {
            const Rec &r = curBook().recs[idx[k]];
            cout << "  " << padRight(itos(k + 1), 6)
                 << padRight(r.date, 12)
                 << padRight(CAT_NAME[r.cat], 10)
                 << padRight(money(r.amount), 12)
                 << cutTo(r.note, 26) << "\n";
        }
        hr();
        cout << "  共 " << idx.size() << " 筆，顯示 " << (start + 1) << "-" << end << " 筆";
        double sum = 0;
        for (size_t i = 0; i < idx.size(); ++i) sum += curBook().recs[idx[i]].amount;
        cout << "｜合計 " << money(sum) << " 元\n";

        if (end >= (int)idx.size()) break;
        int pick;
        cout << "  1) 下一頁   2) 結束瀏覽\n";
        if (!readInt("  選擇：", 1, 2, pick)) break;
        if (pick == 2) break;
        start = end;
    }
    return idx;
}

// 統計：總額、筆數、平均、最花錢類別、各類別小計
static void showStats(const vector<int> &idx, const string &title) {
    double total = 0;
    double byCat[CAT_N];
    int cntCat[CAT_N];
    for (int i = 0; i < CAT_N; ++i) { byCat[i] = 0; cntCat[i] = 0; }
    for (size_t i = 0; i < idx.size(); ++i) {
        const Rec &r = curBook().recs[idx[i]];
        total += r.amount;
        byCat[r.cat] += r.amount;
        cntCat[r.cat] += 1;
    }
    hr('=');
    cout << "  " << title << "\n";
    hr();
    cout << "  總支出：" << money(total) << " 元\n";
    cout << "  筆數：" << idx.size() << " 筆\n";
    if (!idx.empty()) cout << "  平均單筆：" << money(total / (double)idx.size()) << " 元\n";

    int top = -1;
    for (int i = 0; i < CAT_N; ++i) if (cntCat[i] > 0 && (top < 0 || byCat[i] > byCat[top])) top = i;
    cout << "  花最多的類別：" << (top < 0 ? "（無）" : string(CAT_NAME[top])) << "\n";

    if (!idx.empty()) {
        hr();
        cout << "  分類佔比\n";
        for (int i = 0; i < CAT_N; ++i) {
            if (cntCat[i] == 0) continue;
            double pct = total > 0 ? byCat[i] / total * 100.0 : 0;
            int bars = (int)(pct / 5);
            string bar;
            for (int k = 0; k < bars; ++k) bar += '#';
            cout << "  " << padRight(CAT_NAME[i], 10) << padRight(money(byCat[i]), 12)
                 << padRight(" ", 2) << bar << " " << money(pct) << "%（" << cntCat[i] << " 筆）\n";
        }
    }
}

/* ===================== 六、預算 ===================== */

// 計算某分類本月已花多少
static double monthSpentCat(int cat) {
    string m = today().substr(0, 7);
    double s = 0;
    vector<Rec> &recs = curBook().recs;
    for (size_t i = 0; i < recs.size(); ++i)
        if (recs[i].cat == cat && recs[i].date.substr(0, 7) == m) s += recs[i].amount;
    return s;
}

// 計算本月總支出
static double monthSpentTotal() {
    string m = today().substr(0, 7);
    double s = 0;
    vector<Rec> &recs = curBook().recs;
    for (size_t i = 0; i < recs.size(); ++i)
        if (recs[i].date.substr(0, 7) == m) s += recs[i].amount;
    return s;
}

// 畫一條進度條（用文字模擬網站的長條）
static string barOf(double pct) {
    int n = (int)(pct / 5);
    if (n > 20) n = 20;
    string s;
    for (int i = 0; i < n; ++i) s += "█";       // 用字串相加，避免多位元組字元常數的問題
    for (int i = n; i < 20; ++i) s += "·";
    return s;
}

// 顯示本月預算使用狀況
static void showBudget() {
    Book &bk = curBook();
    hr('=');
    cout << "  本月預算（" << today().substr(0, 7) << "）\n";
    hr();
    double spent = monthSpentTotal();
    if (bk.budgetTotal > 0) {
        double pct = spent / bk.budgetTotal * 100.0;
        cout << "  總預算：" << money(bk.budgetTotal) << " 元｜已用 " << money(spent)
             << " 元（" << money(pct) << "%）｜剩 " << money(bk.budgetTotal - spent) << " 元\n";
        cout << "  [" << barOf(pct) << "]\n";
    } else {
        cout << "  總預算：尚未設定（本月已花 " << money(spent) << " 元）\n";
    }
    bool any = false;
    for (int i = 0; i < CAT_N; ++i) {
        if (bk.budgetCat[i] <= 0) continue;
        any = true;
        double used = monthSpentCat(i);
        double pct = used / bk.budgetCat[i] * 100.0;
        cout << "  " << padRight(CAT_NAME[i], 10) << "[" << barOf(pct) << "] "
             << money(used) << " / " << money(bk.budgetCat[i]) << " 元（" << money(pct) << "%）\n";
    }
    if (!any) cout << "  分類預算：尚未設定\n";
}

// 記完一筆後檢查是否跨過 80% / 100%，每道門檻只提示一次
static void checkBudgetAlert() {
    Book &bk = curBook();
    if (bk.budgetTotal > 0) {
        double pct = monthSpentTotal() / bk.budgetTotal * 100.0;
        if (pct >= 100 && !g_alertTotal100) {
            g_alertTotal100 = true; g_alertTotal80 = true;
            cout << "  ⚠ 本月總預算已用完（" << money(pct) << "%），接下來要省一點。\n";
        } else if (pct >= 80 && !g_alertTotal80) {
            g_alertTotal80 = true;
            cout << "  ⚠ 本月總預算已用到 " << money(pct) << "%，剩 "
                 << money(bk.budgetTotal - monthSpentTotal()) << " 元。\n";
        }
    }
    for (int i = 0; i < CAT_N; ++i) {
        if (bk.budgetCat[i] <= 0) continue;
        double pct = monthSpentCat(i) / bk.budgetCat[i] * 100.0;
        if (pct >= 100 && !g_alertCat100[i]) {
            g_alertCat100[i] = true; g_alertCat80[i] = true;
            cout << "  ⚠「" << CAT_NAME[i] << "」分類預算已超支（" << money(pct) << "%）。\n";
        } else if (pct >= 80 && !g_alertCat80[i]) {
            g_alertCat80[i] = true;
            cout << "  ⚠「" << CAT_NAME[i] << "」分類預算已用到 " << money(pct) << "%。\n";
        }
    }
}

// 設定預算的子選單
static void budgetMenu() {
    while (true) {
        hr('=');
        cout << "  預算設定（" << curBook().name << "）\n";
        hr();
        showBudget();
        hr();
        cout << "  1) 設定每月總預算\n";
        cout << "  2) 設定分類預算\n";
        cout << "  3) 清除全部預算\n";
        cout << "  0) 回上一層\n";
        int pick;
        if (!readInt("  選擇：", 0, 3, pick)) return;
        if (pick == 0) return;
        if (pick == 1) {
            double v;
            cout << "  （輸入 0 表示取消總預算）\n";
            if (!readAmountAllowZero("  每月總預算：", v)) return;
            curBook().budgetTotal = v;
            resetAlerts();
            saveData();
            cout << "  ✔ 已設定總預算 " << money(v) << " 元\n";
        } else if (pick == 2) {
            cout << "  選分類：\n";
            for (int i = 0; i < CAT_N; ++i) cout << "   " << (i + 1) << ") " << CAT_NAME[i] << "\n";
            int c;
            if (!readInt("  分類編號：", 1, CAT_N, c)) return;
            double v;
            if (!readAmountAllowZero("  這個分類的預算（0 表示取消）：", v)) return;
            curBook().budgetCat[c - 1] = v;
            resetAlerts();
            saveData();
            cout << "  ✔ 已設定「" << CAT_NAME[c - 1] << "」預算 " << money(v) << " 元\n";
        } else if (pick == 3) {
            curBook().budgetTotal = 0;
            for (int i = 0; i < CAT_N; ++i) curBook().budgetCat[i] = 0;
            resetAlerts();
            saveData();
            cout << "  ✔ 已清除全部預算\n";
        }
        pause();
    }
}

/* ===================== 七、主要動作 ===================== */

// 選類別的小工具（回傳 0..CAT_N-1）
static bool pickCat(int &cat) {
    cout << "  類別：\n";
    for (int i = 0; i < CAT_N; ++i) {
        cout << "   " << setw(2) << (i + 1) << ") " << CAT_NAME[i];
        if ((i + 1) % 5 == 0) cout << "\n"; else cout << "   ";
    }
    cout << "\n";
    int c;
    if (!readInt("  選類別編號：", 1, CAT_N, c)) return false;
    cat = c - 1;
    return true;
}

// 1) 記一筆
static void addRecord() {
    hr('=');
    cout << "  記一筆消費（" << curBook().name << "）\n";
    hr();
    string d;
    string def = today();
    cout << "  日期預設今天（" << def << "），直接按 Enter 就用今天\n";
    if (!readLine("  日期（YYYY-MM-DD）：", d)) return;
    if (d.empty()) d = def;
    if (!isDate(d)) { cout << "  ⚠ 日期格式不對，請用 YYYY-MM-DD。\n"; pause(); return; }
    if (d > def) { cout << "  ⚠ 日期不能晚於今天。\n"; pause(); return; }

    int cat;
    if (!pickCat(cat)) return;

    double amount;
    if (!readAmount("  金額（元）：", amount)) return;

    string note;
    if (!readLine("  備註（可留空）：", note)) return;

    Rec r;
    r.date = d; r.cat = cat; r.amount = amount; r.note = note;
    curBook().recs.push_back(r);       // 直接附加到本帳
    saveData();                        // 立刻存檔，關掉程式也不怕
    cout << "  ✔ 已記下：" << d << "  " << CAT_NAME[cat] << "  " << money(amount) << " 元";
    if (!note.empty()) cout << "（" << note << "）";
    cout << "\n";
    checkBudgetAlert();
    pause();
}

// 2) 看紀錄（含條件篩選）
static void listRecords() {
    int cat = -1;
    string month, q;
    hr('=');
    cout << "  消費紀錄｜目前帳本：" << curBook().name << "（共 " << curBook().recs.size() << " 筆）\n";
    hr();
    cout << "  1) 全部   2) 只看某個月   3) 只看某分類   4) 關鍵字搜尋   5) 篩選條件疊加\n";
    int pick;
    if (!readInt("  選擇：", 1, 5, pick)) return;
    if (pick == 2) {
        string m;
        if (!readLine("  月份（YYYY-MM，例如 2026-11）：", m)) return;
        if (!isDate(m + "-01")) { cout << "  ⚠ 月份格式不對。\n"; pause(); return; }
        month = m;
    } else if (pick == 3) {
        if (!pickCat(cat)) return;
    } else if (pick == 4) {
        if (!readLine("  關鍵字（類別或備註）：", q)) return;
    } else if (pick == 5) {
        string m;
        if (!readLine("  月份（可留空）：", m)) return;
        if (!m.empty()) { if (!isDate(m + "-01")) m = ""; else month = m; }
        if (!pickCat(cat)) return;
        if (!readLine("  關鍵字（可留空）：", q)) return;
    }
    vector<int> idx = collectIdx(cat, month, q);
    printList(idx);
    if (!idx.empty()) {
        if (!readLine("  要看統計嗎？（y/n）：", q)) return;
        if (q == "y" || q == "Y") showStats(idx, "本次篩選統計");
    }
    pause();
}

// 3) 編輯或刪除某筆紀錄
static void editOrDelete() {
    vector<int> idx = collectIdx(-1, "", "");
    if (idx.empty()) { cout << "  目前沒有紀錄可編輯。\n"; pause(); return; }
    printList(idx);                       // 先顯示編號，讓使用者填編號
    int no;
    if (!readInt("  要處理的編號（0 取消）：", 0, (int)idx.size(), no)) return;
    if (no == 0) return;
    int realIdx = idx[no - 1];            // 畫面上的編號 → 實際 vector 索引
    Rec &r = curBook().recs[realIdx];

    hr();
    cout << "  目前：" << r.date << "  " << CAT_NAME[r.cat] << "  " << money(r.amount) << " 元"
         << (r.note.empty() ? "" : "（" + r.note + "）") << "\n";
    hr();
    cout << "  1) 編輯內容   2) 刪除這筆   0) 取消\n";
    int pick;
    if (!readInt("  選擇：", 0, 2, pick)) return;
    if (pick == 1) {
        string d;
        if (!readLine("  新日期（Enter 不改）：", d)) return;
        if (!d.empty()) {
            if (!isDate(d) || d > today()) { cout << "  ⚠ 日期不合法，維持原值。\n"; }
            else r.date = d;
        }
        cout << "  要改類別嗎？（y/n）：";
        string yn;
        if (!readLine("", yn)) return;
        if (yn == "y" || yn == "Y") {
            int c;
            if (pickCat(c)) r.cat = c;
        }
        string a;
        if (!readLine("  新金額（Enter 不改）：", a)) return;
        if (!a.empty()) {
            double v = atof(a.c_str());
            if (v > 0) r.amount = v; else cout << "  ⚠ 金額無效，維持原值。\n";
        }
        string n;
        if (!readLine("  新備註（Enter 不改）：", n)) return;
        if (!n.empty()) r.note = n;
        saveData();
        cout << "  ✔ 已更新。\n";
        checkBudgetAlert();
    } else if (pick == 2) {
        string yn;
        if (!readLine("  確定刪除？（y/n）：", yn)) return;
        if (yn == "y" || yn == "Y") {
            curBook().recs.erase(curBook().recs.begin() + realIdx);
            saveData();
            cout << "  ✔ 已刪除。\n";
        } else {
            cout << "  已取消。\n";
        }
    }
    pause();
}

// 4) 統計
static void statsMenu() {
    hr('=');
    cout << "  統計\n";
    hr();
    cout << "  1) 本月統計   2) 本年統計   3) 全部紀錄   0) 回主選單\n";
    int pick;
    if (!readInt("  選擇：", 0, 3, pick)) return;
    if (pick == 0) return;
    string month, title;
    if (pick == 1) { month = today().substr(0, 7); title = "本月統計（" + month + "）"; }
    else if (pick == 2) { month = today().substr(0, 4); title = "本年統計（" + month + "）"; }
    else { title = "全部紀錄統計（" + curBook().name + "）"; }

    vector<int> idx;
    vector<Rec> &recs = curBook().recs;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (!month.empty()) {
            if (pick == 1 && recs[i].date.substr(0, 7) != month) continue;
            if (pick == 2 && recs[i].date.substr(0, 4) != month) continue;
        }
        idx.push_back((int)i);
    }
    showStats(idx, title);
    pause();
}

// 5) 搜尋
static void searchMenu() {
    string q;
    if (!readLine("  搜尋關鍵字（類別或備註）：", q)) return;
    if (q.empty()) return;
    vector<int> idx = collectIdx(-1, "", q);
    cout << "\n  找到 " << idx.size() << " 筆\n";
    printList(idx);
    pause();
}

// 6) 匯出 CSV（Excel 可直接開啟；檔首加 BOM 避免中文亂碼）
static void exportCsv() {
    vector<int> idx = collectIdx(-1, "", "");
    ofstream f("ledger_export.csv");
    if (!f) { cout << "  ⚠ 無法建立 CSV。\n"; pause(); return; }
    f << "\xEF\xBB\xBF";                       // UTF-8 BOM
    f << "日期,類別,金額,備註\n";
    double sum = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
        const Rec &r = curBook().recs[idx[i]];
        string note;
        for (size_t k = 0; k < r.note.size(); ++k) {
            if (r.note[k] == ',') note += "，";   // 半形逗號換成全形，避免 CSV 欄位錯亂
            else note += r.note[k];
        }
        f << r.date << "," << CAT_NAME[r.cat] << "," << money(r.amount) << "," << note << "\n";
        sum += r.amount;
    }
    f << "合計,,," << money(sum) << "\n";
    f.close();
    cout << "  ✔ 已匯出 ledger_export.csv（" << idx.size() << " 筆，合計 " << money(sum) << " 元）\n";
    pause();
}

/* ===================== 八、帳本管理 ===================== */

// 列出所有帳本並標記目前使用中
static void printBooks() {
    hr('=');
    cout << "  我的帳本（上限 20 本）\n";
    hr();
    cout << "  " << padRight("編號", 6) << padRight("名稱", 20) << padRight("類型", 10)
         << padRight("筆數", 8) << "本月支出\n";
    hr();
    string m = today().substr(0, 7);
    for (size_t i = 0; i < g_books.size(); ++i) {
        double s = 0;
        for (size_t k = 0; k < g_books[i].recs.size(); ++k)
            if (g_books[i].recs[k].date.substr(0, 7) == m) s += g_books[i].recs[k].amount;
        cout << "  " << padRight(itos((int)i + 1), 6)
             << padRight(cutTo(g_books[i].name, 18), 20)
             << padRight(BOOK_TYPE[g_books[i].type], 10)
             << padRight(itos((int)g_books[i].recs.size()), 8)
             << money(s);
        if ((int)i == g_cur) cout << "   ← 使用中";
        cout << "\n";
    }
}

// 切換帳本
static void switchBook() {
    printBooks();
    int no;
    if (!readInt("  切換到第幾本（0 取消）：", 0, (int)g_books.size(), no)) return;
    if (no == 0) return;
    g_cur = no - 1;
    resetAlerts();
    saveData();
    cout << "  ✔ 已切換到「" << curBook().name << "」\n";
    pause();
}

// 新增帳本
static void addBook() {
    if ((int)g_books.size() >= 20) { cout << "  ⚠ 帳本已達上限 20 本。\n"; pause(); return; }
    string name;
    if (!readLine("  帳本名稱：", name)) return;
    if (name.empty()) { cout << "  ⚠ 名稱不能空白。\n"; pause(); return; }
    cout << "  類型：1) 日常  2) 旅遊  3) 專案  4) 公司  5) 其他\n";
    int t;
    if (!readInt("  選類型：", 1, 5, t)) return;
    Book bk;
    bk.name = name;
    bk.type = t - 1;
    g_books.push_back(bk);
    g_cur = (int)g_books.size() - 1;
    resetAlerts();
    saveData();
    cout << "  ✔ 已建立「" << name << "」並切換過去。\n";
    pause();
}

// 改名
static void renameBook() {
    printBooks();
    int no;
    if (!readInt("  要改名的帳本編號（0 取消）：", 0, (int)g_books.size(), no)) return;
    if (no == 0) return;
    string name;
    if (!readLine("  新名稱：", name)) return;
    if (name.empty()) { cout << "  ⚠ 名稱不能空白。\n"; pause(); return; }
    g_books[no - 1].name = name;
    saveData();
    cout << "  ✔ 已改名。\n";
    pause();
}

// 刪除帳本（至少保留一本，且要有二次確認）
static void deleteBook() {
    if ((int)g_books.size() <= 1) { cout << "  ⚠ 至少要保留一本帳。\n"; pause(); return; }
    printBooks();
    int no;
    if (!readInt("  要刪除的帳本編號（0 取消）：", 0, (int)g_books.size(), no)) return;
    if (no == 0) return;
    cout << "  即將刪除「" << g_books[no - 1].name << "」，共 " << g_books[no - 1].recs.size() << " 筆紀錄。\n";
    string yn;
    if (!readLine("  確定？（輸入 YES 才會刪）：", yn)) return;
    if (yn != "YES") { cout << "  已取消。\n"; pause(); return; }
    // 先把資料寫到備份檔，再真的刪除，避免手滑
    ofstream bak("ledger_deleted_backup.txt", ios::app);
    if (bak) {
        bak << "== 刪除帳本：" << g_books[no - 1].name << " ==\n";
        for (size_t i = 0; i < g_books[no - 1].recs.size(); ++i) {
            const Rec &r = g_books[no - 1].recs[i];
            bak << r.date << "|" << CAT_NAME[r.cat] << "|" << money(r.amount) << "|" << r.note << "\n";
        }
        bak.close();
    }
    g_books.erase(g_books.begin() + (no - 1));
    if (g_cur >= (int)g_books.size()) g_cur = (int)g_books.size() - 1;
    saveData();
    cout << "  ✔ 已刪除（原紀錄另存於 ledger_deleted_backup.txt）。\n";
    pause();
}

// 帳本選單
static void booksMenu() {
    while (true) {
        printBooks();
        cout << "\n  1) 切換帳本   2) 新增帳本   3) 帳本改名   4) 刪除帳本   0) 回主選單\n";
        int pick;
        if (!readInt("  選擇：", 0, 4, pick)) return;
        if (pick == 0) return;
        else if (pick == 1) switchBook();
        else if (pick == 2) addBook();
        else if (pick == 3) renameBook();
        else if (pick == 4) deleteBook();
    }
}

/* ===================== 九、備份與說明 ===================== */

// 把資料檔複製成帶日期的備份檔
static void backupData() {
    saveData();
    time_t t = time(0);
    struct tm *lt = localtime(&t);
    char name[64];
    sprintf(name, "ledger_backup_%04d%02d%02d.txt", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    ifstream in(DATA_FILE, ios::binary);
    ofstream out(name, ios::binary);
    if (!in || !out) { cout << "  ⚠ 備份失敗。\n"; pause(); return; }
    out << in.rdbuf();                    // 整個檔案照抄
    in.close(); out.close();
    cout << "  ✔ 已備份為 " << name << "\n";
    pause();
}

// 說明頁
static void helpPage() {
    hr('=');
    cout << "  使用說明\n";
    hr();
    cout << "  · 所有紀錄都存在程式資料夾的 " << DATA_FILE << "，關掉程式不會不見。\n";
    cout << "  · 每天記一筆就好：日期直接 Enter（今天）→ 選類別 → 打金額 → 備註可空白。\n";
    cout << "  · 想看錢花去哪：主選單 4 統計，先看本月，再看分類佔比。\n";
    cout << "  · 想控制支出：主選單 5 預算，設總預算或分類預算，用到 80%／100% 會提示。\n";
    cout << "  · 換手機或換電腦：先 6 匯出 CSV，再把 " << DATA_FILE << " 一起帶走。\n";
    cout << "  · 多本帳：主選單 7 帳本，日常、旅遊、公司報帳可以分開放。\n";
    hr();
    pause();
}

/* ===================== 十、主程式 ===================== */

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);            // 讓中文以 UTF-8 輸出
    SetConsoleCP(65001);                  // 讓中文輸入也走 UTF-8
#endif
    resetAlerts();
    loadData();

    while (true) {
        hr('=');
        cout << "  生活記帳簿（命令列版）｜帳本：" << curBook().name
             << "｜" << curBook().recs.size() << " 筆\n";
        cout << "  今天：" << today() << "｜本月已花：" << money(monthSpentTotal()) << " 元";
        if (curBook().budgetTotal > 0) {
            cout << "｜預算 " << money(curBook().budgetTotal) << " 元";
        }
        cout << "\n";
        hr();
        cout << "  1) 記一筆          2) 看紀錄（含篩選）   3) 編輯／刪除\n";
        cout << "  4) 統計            5) 預算               6) 匯出 CSV\n";
        cout << "  7) 帳本管理        8) 搜尋               9) 備份\n";
        cout << "  h) 使用說明        0) 離開\n";
        hr();

        string choice;
        if (!readLine("  請選擇：", choice)) {
            // 使用者按 Ctrl+Z / 管線結束：直接存檔離開，不留半截資料
            cout << "\n（輸入結束，已存檔離開）\n";
            saveData();
            return 0;
        }
        if (choice == "h" || choice == "H") { helpPage(); continue; }
        bool numeric = !choice.empty();          // 「h」之外只接受數字
        for (size_t i = 0; i < choice.size(); ++i)
            if (!isdigit((unsigned char)choice[i])) { numeric = false; break; }
        if (!numeric) { cout << "  ⚠ 請輸入選單上的數字。\n"; pause(); continue; }

        int pick = atoi(choice.c_str());
        if (pick < 0 || pick > 9) { cout << "  ⚠ 請輸入 0 到 9。\n"; pause(); continue; }
        if (pick == 0) {
            saveData();
            cout << "  已存檔，下次見。\n";
            return 0;
        }
        switch (pick) {
        case 1: addRecord(); break;
        case 2: listRecords(); break;
        case 3: editOrDelete(); break;
        case 4: statsMenu(); break;
        case 5: budgetMenu(); break;
        case 6: exportCsv(); break;
        case 7: booksMenu(); break;
        case 8: searchMenu(); break;
        case 9: backupData(); break;
        default: break;
        }
    }
}
