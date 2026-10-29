// Goose compiler — `goose build` (docs/design/packages.md §6): the C the
// compiler wrote, the runtime object and the native modules' response files,
// compiled and linked into an executable by the toolchain of toolchain.h.
// Intermediates live under GOOSE_HOME; the executable lands beside the root
// source unless -o says otherwise.
#pragma once

namespace goose {

// In main.cpp: the runtime's text hash, the runtime object's C, and a path's directory.
string RuntimeVersion();
string RuntimeObjectSource();
string DirOf(const string &path);

struct BuildOptions {
    bool debug = false;       // --debug: GS_DEBUG=1, debug information, no C optimization
    bool verbose = false;     // -v: print every command and what it prints
    bool dryrun = false;      // --dry-run: print the commands and run nothing
    bool standalone = false;  // --standalone: no runtime object
    string exe;               // -o: the executable; "" puts it beside the root source
};

inline string AbsolutePath(const string &path) {
    std::error_code ec;
    auto abs = filesystem::absolute(filesystem::path(path), ec);
    return ec ? path : abs.lexically_normal().string();
}

// GOOSE_HOME/build/prog/<stem>-<hash of the root's absolute path>/: where a
// program's C, objects and logs go.
inline string ProgramBuildDir(const string &rootfile) {
    auto abs = AbsolutePath(rootfile);
    #ifdef _WIN32
        for (auto &c : abs) c = (char)tolower((unsigned char)c);   // One directory per file, however spelled.
    #endif
    return cat(GooseHome(), "/build/prog/", PathStem(rootfile), "-", Hex8(abs));
}

// Runs the toolchain's commands, printing them as -v and --dry-run ask, and
// a failed one with what it printed.
struct Builder {
    const Toolchain &tc;
    const BuildOptions &opts;
    FILE *msgs;

    // The compile flags of both styles: optimization, or debug information
    // and none of it. Defines go into the C itself (main.cpp), never here.
    vector<string> CFlags() const {
        if (tc.style == "msvc") {
            vector<string> f = { "/nologo" };
            if (opts.debug) f.push_back("/Od"), f.push_back("/Zi");
            else f.push_back("/O2");
            return f;
        }
        vector<string> f = TargetFlags();
        if (opts.debug) f.push_back("-O0"), f.push_back("-g");
        else f.push_back("-O2");
        return f;
    }

    // The cc style's target: this compiler's own architecture on macOS (AppleArch).
    vector<string> TargetFlags() const {
        if (tc.style == "msvc" || !*AppleArch()) return {};
        return { "-arch", AppleArch() };
    }

    // The system libraries a program links beyond the C library, in the cc
    // style: none on Windows, where they live in the C runtime.
    vector<string> SystemLibs() const {
        #ifdef _WIN32
            return {};
        #else
            if (tc.style == "msvc") return {};
            vector<string> libs = { "-lm", "-pthread" };
            #ifdef __linux__
                // dlopen, for the runtime's dynamic externs: in libdl before
                // glibc 2.34, and in libc since, where the archive left
                // behind is empty.
                libs.push_back("-ldl");
            #endif
            return libs;
        #endif
    }

    // Runs `argv`, `what` being what it does ("compiling the runtime"), with
    // its output kept in `logfile`. False when it failed, after saying so.
    bool Run(const vector<string> &argv, const string &what, const string &logfile) const {
        if (opts.dryrun || opts.verbose) fprintf(msgs, "%s\n", CommandLineText(argv).c_str());
        if (opts.dryrun) return true;
        fflush(msgs);
        ProcessResult r;
        {
            ProgressStep step(what);
            r = RunProcess(argv, "", opts.verbose);
        }
        if (!logfile.empty()) WriteFileMakingDirs(logfile, r.output);
        if (r.started && r.code == 0) return true;
        auto msg = cat(what, " failed", r.started ? cat(" (exit ", r.code, ")") : string(), ":\n    ",
                       CommandLineText(argv), "\n");
        if (!opts.verbose) msg += r.output;
        if (!logfile.empty())
            Append(msg, msg.back() == '\n' ? "" : "\n", "(the output is in ", logfile, ")");
        ProgressPrint(msg);
        return false;
    }

    // A system library by name, as the style's linker takes it.
    string LibFlag(const string &lib) const {
        return tc.style == "msvc" ? cat(lib, ".lib") : cat("-l", lib);
    }

    // Compiles the C file `src` to the object `obj`, with `extra` flags
    // (a package's includes and defines) after the usual ones.
    bool CompileObject(const string &src, const string &obj, const string &what,
                       const string &logfile, const vector<string> &extra = {}) const {
        vector<string> argv = { tc.cc };
        for (auto &f : CFlags()) argv.push_back(f);
        for (auto &f : extra) argv.push_back(f);
        if (tc.style == "msvc") {
            argv.push_back("/c");
            argv.push_back(src);
            argv.push_back(cat("/Fo:", obj));
            if (opts.debug) argv.push_back(cat("/Fd:", obj, ".pdb"));
        } else {
            argv.push_back("-c");
            argv.push_back(src);
            argv.push_back("-o");
            argv.push_back(obj);
        }
        return Run(argv, what, logfile);
    }

    // Links `objects` and the system libraries `libs` (LibFlag spellings)
    // into the shared library `out`, with `builddir` taking the
    // intermediates: a package's JIT library.
    bool LinkShared(const vector<string> &objects, const vector<string> &libs, const string &out,
                    const string &builddir, const string &logfile, const string &what) const {
        vector<string> argv = { tc.cc };
        if (tc.style == "msvc") {
            argv.push_back("/nologo");
            argv.push_back("/LD");
            argv.push_back(cat("/Fe:", out));
            argv.push_back(cat("/Fo:", builddir, "\\"));
            for (auto &o : objects) argv.push_back(o);
            for (auto &l : libs) argv.push_back(l);
        } else {
            argv.push_back("-shared");
            for (auto &f : TargetFlags()) argv.push_back(f);
            for (auto &o : objects) argv.push_back(o);
            argv.push_back("-o");
            argv.push_back(out);
            for (auto &l : libs) argv.push_back(l);
            for (auto &l : SystemLibs()) argv.push_back(l);
        }
        return Run(argv, what, logfile);
    }

    // Compiles `src` ("" for none: a link alone) and links it with `objects` and the response files
    // `rsps` into `exe`, with `builddir` taking the intermediates.
    bool Link(const string &src, const vector<string> &objects, const vector<string> &rsps,
              const string &exe, const string &builddir, const string &logfile) const {
        vector<string> argv = { tc.cc };
        for (auto &f : CFlags()) argv.push_back(f);
        if (tc.style == "msvc") {
            argv.push_back(cat("/Fe:", exe));
            argv.push_back(cat("/Fo:", builddir, "\\"));
            if (opts.debug) argv.push_back(cat("/Fd:", builddir, "\\", PathStem(exe), ".pdb"));
            if (!src.empty()) argv.push_back(src);
            for (auto &o : objects) argv.push_back(o);
            for (auto &r : rsps) argv.push_back(cat("@", r));
        } else {
            if (!src.empty()) argv.push_back(src);
            for (auto &o : objects) argv.push_back(o);
            argv.push_back("-o");
            argv.push_back(exe);
            for (auto &r : rsps) argv.push_back(cat("@", r));
            for (auto &l : SystemLibs()) argv.push_back(l);
        }
        return Run(argv, cat("building ", exe), logfile);
    }
};

// An object compiled once per runtime, toolchain and configuration from
// the C `source` gives on first use, and shared by every build after:
// GOOSE_HOME/build/runtime/<runtime hash>/<toolchain id>-<config>/<name>.o,
// its compiler's output in `logname`. "" when it could not be built, after
// the builder said why.
inline string CachedObject(const Builder &b, const string &name, const std::function<string()> &source,
                           const string &what, const string &logname) {
    auto dir = cat(GooseHome(), "/build/runtime/", RuntimeVersion(), "/", b.tc.id, "-",
                   b.opts.debug ? "debug" : "release");
    auto ext = b.tc.style == "msvc" ? "obj" : "o";
    auto obj = cat(dir, "/", name, ".", ext);
    if (PathExists(obj) && !b.opts.dryrun) return obj;
    auto src = cat(dir, "/", name, ".c");
    auto log = cat(dir, "/", logname);
    if (b.opts.dryrun) {
        b.CompileObject(src, obj, what, log);
        return obj;
    }
    std::error_code ec;
    filesystem::create_directories(filesystem::path(dir), ec);
    if (ec) throw CompileError { cat("cannot create ", dir, ": ", ec.message()) };
    // Every first build shares these paths. Wait before writing the source
    // or invoking the compiler, then recheck what the previous builder left.
    FileLock lock(cat(dir, "/build.lock"), cat("waiting for another goose ", what));
    if (PathExists(obj)) return obj;
    ProgressStep step(cat(what, " for ", b.tc.desc), true);
    if (!WriteFileMakingDirs(src, source()))
        throw CompileError { cat("cannot write ", src) };
    // A failed or interrupted compiler may leave a partial object. Only a
    // successful compile publishes the name readers use, by atomic rename.
    auto tmp = cat(dir, "/", name, ".tmp.", ext);
    filesystem::remove(filesystem::path(tmp), ec);
    if (ec) throw CompileError { cat("cannot remove ", tmp, ": ", ec.message()) };
    if (!b.CompileObject(src, tmp, what, log)) return "";
    ec = RenamePath(tmp, obj);
    if (ec) throw CompileError { cat("cannot publish ", obj, ": ", ec.message()) };
    return obj;
}

// The runtime object for this toolchain and configuration, compiled from
// `goose --emit-runtime`'s C on first use (goose_runtime.o).
inline string RuntimeObject(const Builder &b) {
    return CachedObject(b, "goose_runtime", RuntimeObjectSource, "compiling the runtime", "cc.log");
}

// What `goose build` does once tinygen has written the program: finds the
// runtime object for the toolchain `tc`, which built the native parts whose
// response files are `nativersps` (native.h and builtin.h, found by the
// caller) in link order, a package's before the packages it requires and the
// program first of all, and links the executable, named by -o, the program's
// sidecar (`output`) or the root file. tinygen's C (`cfile`, tinygen-c) is
// compiled by the link command; a native tinygen object (`cfile` "", in
// `extraobjects`) links with the support object `tgsupport` names
// (TinygenSupportSource), cached as the runtime object is. The exit code.
inline int BuildExecutable(const string &rootfile, const string &cfile, const string &builddir,
                           const Toolchain &tc, const BuildOptions &opts, FILE *msgs,
                           const vector<string> &nativersps, const string &output,
                           const vector<string> &extraobjects = {},
                           const pair<string, std::function<string()>> &tgsupport = {}) {
    Builder b { tc, opts, msgs };
    // The tinygen object and its support object, then the runtime.
    vector<string> objects = extraobjects;
    if (tgsupport.second) {
        auto obj = CachedObject(b, tgsupport.first, tgsupport.second,
                                "compiling the tinygen support", cat(tgsupport.first, ".log"));
        if (obj.empty()) return 1;
        objects.push_back(obj);
    }
    if (!opts.standalone) {
        auto obj = RuntimeObject(b);
        if (obj.empty()) return 1;
        objects.push_back(obj);
    }
    auto exe = opts.exe;
    if (exe.empty()) {
        exe = cat(DirOf(rootfile), output.empty() ? PathStem(rootfile) : output);
        #ifdef _WIN32
            if (exe.size() < 4 || exe.compare(exe.size() - 4, 4, ".exe") != 0) exe += ".exe";
        #endif
    }
    if (!opts.dryrun) {
        std::error_code ec;
        auto exedirpath = filesystem::path(exe).parent_path();
        if (!exedirpath.empty()) filesystem::create_directories(exedirpath, ec);
    }
    if (!b.Link(cfile, objects, nativersps, exe, builddir, cat(builddir, "/cc.log"))) return 1;
    if (!opts.dryrun) fprintf(msgs, "built %s\n", exe.c_str());
    return 0;
}

}  // namespace goose
