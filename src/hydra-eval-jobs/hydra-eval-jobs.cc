#include <map>
#include <iostream>

#include <gc/gc_allocator.h>

#include "shared.hh"
#include "store-api.hh"
#include "eval.hh"
#include "eval-inline.hh"
#include "util.hh"
#include "json.hh"
#include "get-drvs.hh"
#include "globals.hh"
#include "common-eval-args.hh"

#include "hydra-config.hh"

using namespace nix;


static Path gcRootsDir;


static void findJobs(EvalState & state, JSONObject & top,
    Bindings & autoArgs, Value & v, const string & attrPath);


static string queryMetaStrings(EvalState & state, DrvInfo & drv, const string & name)
{
    Strings res;
    std::function<void(Value & v)> rec;

    rec = [&](Value & v) {
        state.forceValue(v);
        if (v.type == tString)
            res.push_back(v.string.s);
        else if (v.isList())
            for (unsigned int n = 0; n < v.listSize(); ++n)
                rec(*v.listElems()[n]);
        else if (v.type == tAttrs) {
            auto a = v.attrs->find(state.symbols.create("shortName"));
            if (a != v.attrs->end())
                res.push_back(state.forceString(*a->value));
        }
    };

    Value * v = drv.queryMeta(name);
    if (v) rec(*v);

    return concatStringsSep(", ", res);
}


static void findJobsWrapped(EvalState & state, JSONObject & top,
    Bindings & autoArgs, Value & vIn, const string & attrPath)
{
    debug(format("at path `%1%'") % attrPath);

    checkInterrupt();

    Value v;
    state.autoCallFunction(autoArgs, vIn, v);

    if (v.type == tAttrs) {

        auto drv = getDerivation(state, v, false);

        if (drv) {
            Path drvPath;

            DrvInfo::Outputs outputs = drv->queryOutputs();

            if (drv->querySystem() == "unknown")
                throw EvalError("derivation must have a ‘system’ attribute");

            {
            auto res = top.object(attrPath);
            res.attr("nixName", drv->queryName());
            res.attr("system", drv->querySystem());
            res.attr("drvPath", drvPath = drv->queryDrvPath());
            res.attr("description", drv->queryMetaString("description"));
            res.attr("license", queryMetaStrings(state, *drv, "license"));
            res.attr("homepage", drv->queryMetaString("homepage"));
            res.attr("maintainers", queryMetaStrings(state, *drv, "maintainers"));
            res.attr("schedulingPriority", drv->queryMetaInt("schedulingPriority", 100));
            res.attr("timeout", drv->queryMetaInt("timeout", 36000));
            res.attr("maxSilent", drv->queryMetaInt("maxSilent", 7200));
            res.attr("isChannel", drv->queryMetaBool("isHydraChannel", false));

            /* If this is an aggregate, then get its constituents. */
            Bindings::iterator a = v.attrs->find(state.symbols.create("_hydraAggregate"));
            if (a != v.attrs->end() && state.forceBool(*a->value, *a->pos)) {
                Bindings::iterator a = v.attrs->find(state.symbols.create("constituents"));
                if (a == v.attrs->end())
                    throw EvalError("derivation must have a ‘constituents’ attribute");
                PathSet context;
                state.coerceToString(*a->pos, *a->value, context, true, false);
                PathSet drvs;
                for (auto & i : context)
                    if (i.at(0) == '!') {
                        size_t index = i.find("!", 1);
                        drvs.insert(string(i, index + 1));
                    }
                res.attr("constituents", concatStringsSep(" ", drvs));
            }

            /* Register the derivation as a GC root.  !!! This
               registers roots for jobs that we may have already
               done. */
            auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();
            if (gcRootsDir != "" && localStore) {
                Path root = gcRootsDir + "/" + baseNameOf(drvPath);
                if (!pathExists(root)) localStore->addPermRoot(drvPath, root, false);
            }

            auto res2 = res.object("outputs");
            for (auto & j : outputs)
                res2.attr(j.first, j.second);

            }
        }

        else {
            if (!state.isDerivation(v)) {
                for (auto & i : *v.attrs)
                    findJobs(state, top, autoArgs, *i.value,
                        (attrPath.empty() ? "" : attrPath + ".") + (string) i.name);
            }
        }
    }

    else if (v.type == tNull) {
        // allow null values, meaning 'do nothing'
    }

    else
        throw TypeError(format("unsupported value: %1%") % v);
}


static void findJobs(EvalState & state, JSONObject & top,
    Bindings & autoArgs, Value & v, const string & attrPath)
{
    try {
        findJobsWrapped(state, top, autoArgs, v, attrPath);
    } catch (EvalError & e) {
        {
        auto res = top.object(attrPath);
        res.attr("error", e.msg());
        }
    }
}


int main(int argc, char * * argv)
{
    /* Prevent undeclared dependencies in the evaluation via
       $NIX_PATH. */
    unsetenv("NIX_PATH");

    return handleExceptions(argv[0], [&]() {

        auto config = std::make_unique<::Config>();

        auto initialHeapSize = config->getStrOption("evaluator_initial_heap_size", "");
        if (initialHeapSize != "")
            setenv("GC_INITIAL_HEAP_SIZE", initialHeapSize.c_str(), 1);

        initNix();
        initGC();

        Path releaseExpr;

        struct MyArgs : LegacyArgs, MixEvalArgs
        {
            using LegacyArgs::LegacyArgs;
        };

        MyArgs myArgs(baseNameOf(argv[0]), [&](Strings::iterator & arg, const Strings::iterator & end) {
            if (*arg == "--gc-roots-dir")
                gcRootsDir = getArg(*arg, arg, end);
            else if (*arg == "--dry-run")
                settings.readOnlyMode = true;
            else if (*arg != "" && arg->at(0) == '-')
                return false;
            else
                releaseExpr = *arg;
            return true;
        });

        myArgs.parseCmdline(argvToStrings(argc, argv));

        /* FIXME: The build hook in conjunction with import-from-derivation is causing "unexpected EOF" during eval */
        settings.builders = "";

        /* Prevent access to paths outside of the Nix search path and
           to the environment. */
        //settings.restrictEval = true;

        if (releaseExpr == "") throw UsageError("no expression specified");

        if (gcRootsDir == "") printMsg(lvlError, "warning: `--gc-roots-dir' not specified");

        EvalState state(myArgs.searchPath, openStore());

        Bindings & autoArgs = *myArgs.getAutoArgs(state);

        Value v;
        state.evalFile(lookupFileArg(state, releaseExpr), v);

        JSONObject json(std::cout, true);
        findJobs(state, json, autoArgs, v, "");

        state.printStats();
    });
}
