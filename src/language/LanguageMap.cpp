#include "LanguageMap.h"
#include <cwctype>
#include <cstring>

namespace xfs {
namespace {

// ---- keyword seeds -------------------------------------------------------
const char* kCppKw =
    "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t "
    "char16_t char32_t class compl concept const consteval constexpr constinit const_cast "
    "continue co_await co_return co_yield decltype default delete do double dynamic_cast "
    "else enum explicit export extern false float for friend goto if inline int long mutable "
    "namespace new noexcept not not_eq nullptr operator or or_eq private protected public "
    "register reinterpret_cast requires return short signed sizeof static static_assert "
    "static_cast struct switch template this thread_local throw true try typedef typeid "
    "typename union unsigned using virtual void volatile wchar_t while xor xor_eq "
    "override final";
const char* kCppTypes =
    "std uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t ssize_t "
    "uintptr_t intptr_t HWND HRESULT DWORD WORD BYTE LPSTR LPCWSTR vector string wstring map "
    "set unordered_map unordered_set unique_ptr shared_ptr weak_ptr optional variant any";

const char* kPythonKw =
    "and as assert async await break class continue def del elif else except False finally "
    "for from global if import in is lambda None nonlocal not or pass raise return True try "
    "while with yield match case";
const char* kPythonBuiltins =
    "abs all any bin bool bytearray bytes callable chr dict dir divmod enumerate eval exec "
    "filter float format frozenset getattr globals hasattr hash hex id input int isinstance "
    "issubclass iter len list map max min next object oct open ord pow print range repr "
    "reversed round set setattr slice sorted staticmethod str sum super tuple type vars zip";

const char* kJsKw =
    "abstract await async break case catch class const continue debugger default delete do "
    "else enum export extends false finally for from function get if implements import in "
    "instanceof interface let new null of private protected public readonly return set static "
    "super switch this throw true try typeof undefined var void while with yield";
const char* kJsTypes =
    "Array Boolean Date Error Function Math Number Object RegExp String JSON Promise Map Set "
    "Symbol BigInt console document window";

const char* kSqlKw =
    "ADD ALL ALTER AND ANY AS ASC AUTHORIZATION BACKUP BEGIN BETWEEN BREAK BROWSE BULK BY "
    "CASCADE CASE CHECK CHECKPOINT CLOSE CLUSTERED COALESCE COLLATE COLUMN COMMIT COMPUTE "
    "CONSTRAINT CONTAINS CONTAINSTABLE CONTINUE CREATE CROSS CURRENT CURRENT_DATE CURRENT_TIME "
    "CURRENT_TIMESTAMP CURRENT_USER CURSOR DATABASE DBCC DEALLOCATE DECLARE DEFAULT DELETE "
    "DENY DESC DISK DISTINCT DISTRIBUTED DOUBLE DROP DUMP ELSE END ERRLVL ESCAPE EXCEPT EXEC "
    "EXECUTE EXISTS EXIT EXTERNAL FETCH FILE FILLFACTOR FOR FOREIGN FREETEXT FREETEXTTABLE "
    "FROM FULL FUNCTION GOTO GRANT GROUP HAVING HOLDLOCK IDENTITY IF IN INDEX INNER INSERT "
    "INTERSECT INTO IS JOIN KEY KILL LEFT LIKE LOAD MERGE NATIONAL NOCHECK NONCLUSTERED NOT "
    "NULL NULLIF OF OFF OFFSETS ON OPEN OPTION OR ORDER OUTER OVER PERCENT PIVOT PLAN "
    "PRECISION PRIMARY PRINT PROC PROCEDURE PUBLIC RAISERROR READ REFERENCES REPLICATION "
    "RESTORE RESTRICT RETURN REVERT REVOKE RIGHT ROLLBACK RULE SAVE SCHEMA SELECT SESSION_USER "
    "SET SETUSER SHUTDOWN SOME STATISTICS SYSTEM_USER TABLE TABLESAMPLE TEXTSIZE THEN TO TOP "
    "TRAN TRANSACTION TRIGGER TRUNCATE TSEQUAL UNION UNIQUE UNPIVOT UPDATE USE USER VALUES "
    "VARYING VIEW WAITFOR WHEN WHERE WHILE WITH WRITETEXT";
const char* kSqlTypes =
    "bigint binary bit char date datetime datetime2 datetimeoffset decimal float image int "
    "money nchar ntext numeric nvarchar real rowversion smalldatetime smallint smallmoney "
    "text time tinyint uniqueidentifier varbinary varchar xml";

const char* kShellKw =
    "if then else elif fi for while until do done case esac in function select time coproc "
    "return exit break continue local export readonly declare unset shift eval exec trap set "
    "echo cd pwd source alias bind builtin command help history jobs kill let logout printf "
    "pushd read shopt suspend test times type ulimit umask unalias wait";

const char* kYamlKw = "true false null yes no on off";
const char* kJsonKw = "true false null";

const char* kBatchKw =
    "echo off cls pause rem set if exist errorlevel for in do goto call shift not else "
    "equ neq lss leq gtr geq defined setlocal endlocal enableextensions delayedexpansion "
    "exit start title color choice timeout del copy move mkdir rmdir ren attrib xcopy "
    "robocopy findstr tasklist taskkill reg net powershell";

const char* kPowershellKw =
    "begin break catch class continue data define do dynamicparam else elseif end enum exit "
    "filter finally for foreach from function hidden if in param process return static switch "
    "throw trap try until using var while workflow parallel sequence configuration";
const char* kPowershellCmdlets =
    "Get-ChildItem Get-Content Set-Content Write-Host Write-Output Out-File Select-Object "
    "Where-Object ForEach-Object Sort-Object Group-Object Measure-Object Compare-Object "
    "New-Item Remove-Item Copy-Item Move-Item Rename-Item Test-Path Join-Path Split-Path "
    "Get-Date Get-Process Stop-Process Start-Process Invoke-Command Invoke-WebRequest "
    "Invoke-RestMethod ConvertTo-Json ConvertFrom-Json Export-Csv Import-Csv Get-Help "
    "Get-Member Get-Command Get-Variable Read-Host Clear-Host Tee-Object";

const char* kPhpKw =
    "abstract and array as break callable case catch class clone const continue declare default "
    "do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile extends "
    "final finally fn for foreach function global goto if implements include include_once "
    "instanceof insteadof interface isset list match namespace new or print private protected "
    "public readonly require require_once return static switch throw trait try unset use var "
    "while xor yield";

const char* kHtmlTags =
    "a abbr acronym address applet area article aside audio b base basefont bdi bdo big "
    "blockquote body br button canvas caption center cite code col colgroup data datalist dd "
    "del details dfn dialog dir div dl dt em embed fieldset figcaption figure font footer form "
    "frame frameset h1 h2 h3 h4 h5 h6 head header hr html i iframe img input ins kbd label "
    "legend li link main map mark menu meta meter nav noscript object ol optgroup option output "
    "p param picture pre progress q rp rt ruby s samp section select slot small source span "
    "strike strong style sub summary sup svg table tbody td template textarea tfoot th thead "
    "time title tr track tt u ul var video wbr";

const char* kCssProps =
    "align-content align-items all animation background border bottom box-sizing clear clip "
    "color column content cursor direction display flex flex-basis flex-direction flex-wrap "
    "float font font-family font-size font-style font-weight grid height justify-content left "
    "letter-spacing line-height list-style margin max-height max-width min-height min-width "
    "object-fit opacity order outline overflow padding position right table-layout text-align "
    "text-decoration text-indent text-overflow text-shadow text-transform top transform "
    "transition vertical-align visibility white-space width word-break word-spacing z-index";

const char* kVerilogKw =
    "always and assign automatic begin buf bufif0 bufif1 case casex casez cell cmos config "
    "deassign default defparam design disable edge else end endcase endconfig endfunction "
    "endgenerate endmodule endprimitive endspecify endtable endtask event for force forever "
    "fork function generate genvar highz0 highz1 if ifnone incdir include initial inout input "
    "instance integer join large liblist library localparam macromodule medium module nand "
    "negedge nmos nor noshowcancelled not notif0 notif1 or output parameter pmos posedge "
    "primitive pull0 pull1 pulldown pullup rcmos real realtime reg release repeat rnmos rpmos "
    "rtran rtranif0 rtranif1 scalared signed small specify specparam strong0 strong1 supply0 "
    "supply1 table task time tran tranif0 tranif1 tri tri0 tri1 triand trior trireg unsigned "
    "use uwire vectored wait wand weak0 weak1 while wire wor xnor xor";
const char* kVhdlKw =
    "abs access after alias all and architecture array assert attribute begin block body "
    "buffer bus case component configuration constant disconnect downto else elsif end entity "
    "exit file for function generate generic group guarded if impure in inertial inout is "
    "label library linkage literal loop map mod nand new next nor not null of on open or "
    "others out package port postponed procedure process pure range record register reject "
    "rem report return rol ror select severity shared signal sla sll sra srl subtype then to "
    "transport type unaffected units until use variable wait when while with xnor xor";

// Table rows are plain LanguageInfo aggregates

#define LANG(exts, lex, kw0, kw1) {exts, lex, {kw0, kw1}}

const wchar_t* eC[]   = {L"c", nullptr};
const wchar_t* eCpp[] = {L"cpp", L"cxx", L"cc", L"h", L"hh", L"hpp", L"hxx", L"inl", nullptr};
const wchar_t* eCs[]  = {L"cs", L"java", nullptr};
const wchar_t* ePy[]  = {L"py", L"pyw", L"pyi", nullptr};
const wchar_t* eJs[]  = {L"js", L"mjs", L"cjs", L"jsx", nullptr};
const wchar_t* eTs[]  = {L"ts", L"tsx", nullptr};
const wchar_t* eHtml[]= {L"html", L"htm", L"xhtml", L"php", nullptr};
const wchar_t* eXml[] = {L"xml", L"xsl", L"xslt", L"svg", L"plist", L"wsdl", L"stil", nullptr};
const wchar_t* eCss[] = {L"css", nullptr};
const wchar_t* eJson[]= {L"json", L"jsonc", nullptr};
const wchar_t* eYaml[]= {L"yaml", L"yml", nullptr};
const wchar_t* eSql[] = {L"sql", nullptr};
const wchar_t* eSh[]  = {L"sh", L"bash", nullptr};
const wchar_t* ePs1[] = {L"ps1", L"psm1", L"psd1", nullptr};
const wchar_t* eBat[] = {L"bat", L"cmd", nullptr};
const wchar_t* eRuby[]= {L"rb", L"ruby", nullptr};
const wchar_t* eGo[]  = {L"go", nullptr};
const wchar_t* eRust[]= {L"rs", nullptr};
const wchar_t* eLua[] = {L"lua", nullptr};
const wchar_t* ePerl[]= {L"pl", L"pm", nullptr};
const wchar_t* ePas[] = {L"pas", L"dpr", nullptr};
const wchar_t* eFor[] = {L"f", L"for", L"f90", L"f95", nullptr};
const wchar_t* eV[]   = {L"v", L"vh", nullptr};
const wchar_t* eVhd[] = {L"vhd", L"vhdl", nullptr};
const wchar_t* eMat[] = {L"m", L"mat", nullptr};
const wchar_t* eMd[]  = {L"md", L"markdown", nullptr};
const wchar_t* eIni[] = {L"ini", L"inf", L"cfg", L"conf", L"config", L"properties", nullptr};
const wchar_t* eToml[]= {L"toml", nullptr};
const wchar_t* eMake[]= {L"mak", L"make", nullptr};
const wchar_t* eCmake[]={L"cmake", nullptr};
const wchar_t* eDiff[]= {L"diff", L"patch", nullptr};
const wchar_t* eAsm[] = {L"asm", L"s", nullptr};

const LanguageInfo g_table[] = {
    LANG(eC,    "cpp",       kCppKw,   kCppTypes),
    LANG(eCpp,  "cpp",       kCppKw,   kCppTypes),
    LANG(eCs,   "cpp",       kCppKw,   kCppTypes),
    LANG(ePy,   "python",    kPythonKw, kPythonBuiltins),
    LANG(eJs,   "cpp",       kJsKw,    kJsTypes),
    LANG(eTs,   "cpp",       kJsKw,    kJsTypes),
    LANG(eHtml, "hypertext", kHtmlTags, kCssProps),
    LANG(eXml,  "xml",       kHtmlTags, nullptr),
    LANG(eCss,  "css",       kCssProps, nullptr),
    LANG(eJson, "json",      kJsonKw,  nullptr),
    LANG(eYaml, "yaml",      kYamlKw,  nullptr),
    LANG(eSql,  "sql",       kSqlKw,   kSqlTypes),
    LANG(eSh,   "bash",      kShellKw, nullptr),
    LANG(ePs1,  "powershell", kPowershellKw, kPowershellCmdlets),
    LANG(eBat,  "batch",     kBatchKw, nullptr),
    LANG(eRuby, "ruby",      nullptr,  nullptr),
    LANG(eGo,   "cpp",       kCppTypes, nullptr),
    LANG(eRust, "rust",      nullptr,  nullptr),
    LANG(eLua,  "lua",       nullptr,  nullptr),
    LANG(ePerl, "perl",      nullptr,  nullptr),
    LANG(ePas,  "pascal",    nullptr,  nullptr),
    LANG(eFor,  "fortran",   nullptr,  nullptr),
    LANG(eV,    "verilog",   kVerilogKw, nullptr),
    LANG(eVhd,  "vhdl",      kVhdlKw,  nullptr),
    LANG(eMat,  "matlab",    nullptr,  nullptr),
    LANG(eMd,   "markdown",  nullptr,  nullptr),
    LANG(eIni,  "props",     nullptr,  nullptr),
    LANG(eToml, "toml",      kYamlKw,  nullptr),
    LANG(eDiff, "diff",      nullptr,  nullptr),
    LANG(eAsm,  "asm",       nullptr,  nullptr),
};

} // namespace

const LanguageInfo* DetectLanguage(const std::wstring& fileName) {
    std::wstring name = fileName;
    size_t slash = name.find_last_of(L"/\\");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);

    // special file names
    if (_wcsicmp(name.c_str(), L"cmakelists.txt") == 0) {
        static const LanguageInfo cm{nullptr, "cmake", {nullptr, nullptr}};
        return &cm;
    }

    size_t dot = name.find_last_of(L'.');
    std::wstring ext;
    if (_wcsnicmp(name.c_str(), L"makefile.", 9) == 0 || _wcsicmp(name.c_str(), L"makefile") == 0)
        ext = L"make";
    else if (dot != std::wstring::npos && dot + 1 < name.size())
        ext = name.substr(dot + 1);
    else
        return nullptr;

    for (auto& c : ext) c = (wchar_t)towlower(c);

    for (const auto& e : g_table) {
        for (int i = 0; e.extensions[i]; ++i) {
            if (ext == e.extensions[i]) return &e;
        }
    }
    // makefile extension handled via dedicated pseudo-entry
    if (ext == L"make") {
        static const LanguageInfo mk{eMake, "makefile", {nullptr, nullptr}};
        return &mk;
    }
    return nullptr;
}

// ---- top-level Language menu catalog -------------------------------------
// Index of each entry = command offset from Cmd::LangFirst.

const LanguageMenuItem kMenuCatalog[] = {
    {L"&Normal Text",          nullptr,      {nullptr,      nullptr}},
    {L"&C/C++",                "cpp",        {kCppKw,       kCppTypes}},
    {L"C#",                    "cpp",        {kCppKw,       kCppTypes}},
    {L"&Java",                 "cpp",        {kCppKw,       kCppTypes}},
    {L"&Python",               "python",     {kPythonKw,    kPythonBuiltins}},
    {L"&JavaScript",           "cpp",        {kJsKw,        kJsTypes}},
    {L"&TypeScript",           "cpp",        {kJsKw,        kJsTypes}},
    {L"&HTML",                 "hypertext",  {kHtmlTags,    kCssProps}},
    {L"X&ML",                  "xml",        {kHtmlTags,    nullptr}},
    {L"&CSS",                  "css",        {kCssProps,    nullptr}},
    {L"&JSON",                 "json",       {kJsonKw,      nullptr}},
    {L"&YAML",                 "yaml",       {kYamlKw,      nullptr}},
    {L"S&QL",                  "sql",        {kSqlKw,       kSqlTypes}},
    {L"&Shell",                "bash",       {kShellKw,     nullptr}},
    {L"P&owerShell",           "powershell", {kPowershellKw, kPowershellCmdlets}},
    {L"&Batch",                "batch",      {kBatchKw,     nullptr}},
    {L"Ru&by",                 "ruby",       {nullptr,      nullptr}},
    {L"&Go",                   "cpp",        {kCppTypes,    nullptr}},
    {L"&Rust",                 "rust",       {nullptr,      nullptr}},
    {L"L&ua",                  "lua",        {nullptr,      nullptr}},
    {L"Pe&rl",                 "perl",       {nullptr,      nullptr}},
    {L"&Pascal",               "pascal",     {nullptr,      nullptr}},
    {L"&Fortran",              "fortran",    {nullptr,      nullptr}},
    {L"&Verilog",              "verilog",    {kVerilogKw,   nullptr}},
    {L"V&HDL",                 "vhdl",       {kVhdlKw,      nullptr}},
    {L"&MATLAB",               "matlab",     {nullptr,      nullptr}},
    {L"&Markdown",             "markdown",   {nullptr,      nullptr}},
    {L"&INI/Properties",       "props",      {nullptr,      nullptr}},
    {L"&TOML",                 "toml",       {kYamlKw,      nullptr}},
    {L"&Makefile",             "makefile",   {nullptr,      nullptr}},
    {L"C&Make",                "cmake",      {nullptr,      nullptr}},
    {L"&Diff",                 "diff",       {nullptr,      nullptr}},
    {L"&Assembler",            "asm",        {nullptr,      nullptr}},
    // null terminator (label == nullptr marks the end of the catalog)
    {nullptr,                  nullptr,      {nullptr,      nullptr}},
};

const LanguageMenuItem* LanguageMenuCatalog() {
    return kMenuCatalog;
}

// 批次 29：lexer 名 → 行注释前缀。表驱动，与上面两个目录同排布习惯。
// 无行注释的语言（css/html/xml/diff/markdown/toml/纯文本）返回 nullptr，
// 「切换行注释」命令对它们不动作。
const char* LineCommentToken(const char* lexerName) {
    if (!lexerName) return nullptr;
    static const struct { const char* lexer; const char* token; } kTokens[] = {
        { "cpp",        "//"   },
        { "python",     "# "   },
        { "hypertext",  nullptr },   // html/php：块注释，不做行注释
        { "xml",        nullptr },
        { "css",        nullptr },
        { "json",       nullptr },   // 标准 json 无注释（jsonc 手动 //）
        { "yaml",       "# "   },
        { "sql",        "-- "  },
        { "bash",       "# "   },
        { "powershell", "# "   },
        { "batch",      ":: "  },   // rem 有尾随空格歧义，:: 通用性够
        { "ruby",       "# "   },
        { "rust",       "//"   },
        { "lua",        "-- "  },
        { "perl",       "# "   },
        { "pascal",     "//"   },
        { "fortran",    "! "   },
        { "verilog",    "//"   },
        { "vhdl",       "-- "  },
        { "matlab",     "% "   },
        { "markdown",   nullptr },
        { "props",      "# "   },   // ini/properties 行注释（; 亦有）
        { "toml",       "# "   },
        { "makefile",   "# "   },
        { "cmake",      "# "   },
        { "diff",       nullptr },
        { "asm",        "; "   },
    };
    for (const auto& e : kTokens)
        if (strcmp(e.lexer, lexerName) == 0) return e.token;
    return nullptr;
}

} // namespace xfs
