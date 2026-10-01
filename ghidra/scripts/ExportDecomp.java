// Decompile every game-specific function and write one C file per top-level namespace.
// Names are demangled with the system c++filt (Ghidra's GNU demangler has no mac_arm_64 build).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        // Functions only reached through vtables / member pointers have a symbol but no
        // function yet: create them from the symbol table.
        int created = 0;
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
        for (Symbol sym : currentProgram.getSymbolTable().getAllSymbols(true)) {
            ghidra.program.model.address.Address a = sym.getAddress();
            if (!a.isMemoryAddress() || sym.getSymbolType() != SymbolType.LABEL) continue;
            ghidra.program.model.mem.MemoryBlock blk = mem.getBlock(a);
            if (blk == null || !blk.isExecute() || !blk.getName().equals("__text")) continue;
            if (!sym.getName().startsWith("_")) continue;
            if (getFunctionContaining(a) != null) continue;
            disassemble(a);
            if (getInstructionAt(a) == null) continue;
            if (createFunction(a, sym.getName()) != null) created++;
        }
        println("created " + created + " functions from symbols");
        List<Function> funcs = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) funcs.add(f);

        // demangle in one c++filt run
        File in = File.createTempFile("names", ".txt");
        try (PrintWriter w = new PrintWriter(in)) {
            for (Function f : funcs) {
                String nm = f.getName();
                w.println(nm);   // macOS c++filt expects the Mach-O form (__Z...)
            }
        }
        Process p = new ProcessBuilder("/usr/bin/c++filt").redirectInput(in).start();
        List<String> dem = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
            String line;
            while ((line = r.readLine()) != null) dem.add(line);
        }
        if (dem.size() != funcs.size()) throw new RuntimeException("c++filt line mismatch " + dem.size() + " vs " + funcs.size());

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Map<String, PrintWriter> files = new HashMap<>();
        PrintWriter index = new PrintWriter(new FileWriter(outDir + "/INDEX.txt"));
        int n = 0, skipped = 0;
        String[] libPrefix = { "b2", "std::", "__gnu", "operator", "lua_", "luaL_", "luaopen", "png_", "inflate", "deflate",
            "INT123", "mpg123", "_lua", "_png", "_inflate", "_deflate", "_INT123", "_mpg123", "___", "__", "_adler", "_crc32",
            "_z", "_dct", "_synth", "_frame_", "_do_", "_updatewindow", "_safe_realloc", "_invalidate", "_samples_to", "_bytes_to" };
        for (int i = 0; i < funcs.size(); i++) {
            Function f = funcs.get(i);
            String full = dem.get(i);
            boolean cxx = f.getName().startsWith("__Z");
            boolean lib = false;
            for (String pre : libPrefix) if (full.startsWith(pre)) { lib = true; break; }
            if (!cxx && f.getName().startsWith("_") && !f.getName().equals("_main")) {
                // plain C: Lua core (luaX_.., lj etc.), libpng, zlib, mpg123
                lib = true;
            }
            // strip return type/templates to find the owning namespace
            String head = full;
            int paren = head.indexOf('(');
            if (paren > 0) head = head.substring(0, paren);
            int sp = head.lastIndexOf(' ');
            if (sp > 0 && !head.contains("operator")) head = head.substring(sp + 1);
            String top = head.contains("::") ? head.substring(0, head.indexOf("::")) : "_global";
            if (top.startsWith("b2") || top.equals("std")) lib = true;
            index.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + (lib ? "LIB" : "GAME") + "\t" + full);
            if (lib) { skipped++; continue; }
            top = top.replaceAll("[^A-Za-z0-9_]", "_");
            if (top.length() > 40) top = top.substring(0, 40);
            PrintWriter w = files.get(top);
            if (w == null) { w = new PrintWriter(new FileWriter(outDir + "/" + top + ".c")); files.put(top, w); }
            DecompileResults r = dec.decompileFunction(f, 180, monitor);
            w.println("// ===== " + full + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
            if (r != null && r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
            else w.println("// DECOMPILE FAILED: " + (r == null ? "null" : r.getErrorMessage()));
            n++;
        }
        for (PrintWriter w : files.values()) w.close();
        index.close();
        println("decompiled " + n + " functions, skipped " + skipped + " library functions");
    }
}
