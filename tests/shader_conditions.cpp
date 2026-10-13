#include "globals.h"
#include "CommandList.h"
#include "shader_conditions.h"
#include "port_api.h"
#include <cstdio>
#include <stdexcept>

namespace MigotoPort { void InitEngineGlobals(); }
extern HINSTANCE migoto_handle;

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int wmain(int argc, wchar_t** argv)
{
    try {
        MigotoPort::InitEngineGlobals();
        const std::wstring ini_namespace = L"mods\\rover";
        CommandListVariable enabled(L"$mod_enabled", 1.f, VariableFlags::GLOBAL);
        CommandListVariable detected(L"$object_detected", 0.f, VariableFlags::GLOBAL);
        CommandListVariable filter(L"$ps", 0.f, VariableFlags::GLOBAL);
        CommandListScope scope{ {{L"$mod_enabled", &enabled}, {L"$object_detected", &detected}, {L"$ps", &filter}} };
        command_list_globals.emplace(L"$\\rover ps costume\\enabled",
            CommandListVariable(L"$\\rover ps costume\\enabled", 1.f, VariableFlags::GLOBAL));
        customResources.try_emplace(L"resource\\rover ps costume\\diffuse");

        struct Case { const wchar_t* expression; bool expected; };
        const Case cases[] = {
            {L"cs == 3381.4444", true},
            {L"$mod_enabled && cs == 3381.4444", true},
            {L"$mod_enabled == 1 && cs == 3381.4444", true},
            {L"3381.4444 == cs", true},
            {L"cs!=3381.4444", true},
            {L"($object_detected && (ps == 1024)) || vs != 0", true},
            {L"!ps", true},
            {L"abs(ps) > 0", true},
            {L"ps-t0 == 1 && cs == 3381.4444", true},
            {L"gs == 1 || hs == 2 || ds == 3", true},
            {L"vs->cb_mask != 0", true},
            {L"$mod_enabled == 1", false},
            {L"$ps == 1", false},
            {L"ps-t0 == 1", false},
            {L"vs-cb0 == 1 || cs-u0 == 2", false},
            {L"$\\rover ps costume\\enabled == 1", false},
            {L"resource\\rover ps costume\\diffuse == 1", false},
            {L"@resource\\rover ps costume\\diffuse != 0", false},
        };
        for (const auto& example : cases) {
            std::wstring text(example.expression);
            CommandListExpression expression;
            if (!expression.parse(&text, &ini_namespace, &scope)) {
                std::fwprintf(stderr, L"Parser rejected: %ls\n", example.expression);
                throw std::runtime_error("invalid expression fixture");
            }
            if (MigotoPort::ExpressionReadsShader(expression.evaluatable.get()) != example.expected) {
                std::fwprintf(stderr, L"Misclassified: %ls\n", example.expression);
                throw std::runtime_error("shader dependency");
            }
        }

        // Real engine commands: nested elif/post lists and linked lists must be
        // inspected without executing them, including cyclic run graphs.
        ExplicitCommandListSection skeleton_update;
        auto condition = std::make_shared<ElseIfCommand>(L"CommandListUpdateMergedSkeleton");
        const std::wstring shader_check = L"$mod_enabled && cs == 3381.4444";
        Check(condition->expression.parse(&shader_check, &ini_namespace, &scope), "nested condition parse");
        skeleton_update.post_command_list.commands.push_back(condition);
        CommandList root;
        auto run = std::make_shared<RunExplicitCommandList>();
        run->command_list_section = &skeleton_update;
        root.commands.push_back(run);
        Check(MigotoPort::ListsReadShaderConditions(&root, nullptr), "explicit post-list condition");
        auto outer = std::make_shared<IfCommand>(L"TextureOverrideRoverShapeKeyCallback");
        outer->false_commands_post = std::make_shared<CommandList>();
        outer->false_commands_post->commands.push_back(condition);
        CommandList nested;
        nested.commands.push_back(outer);
        Check(MigotoPort::ListsReadShaderConditions(&nested, nullptr), "nested false/post elif");
        MigotoPort::WrapObservedList(&nested);
        Check(MigotoPort::ListsReadShaderConditions(&nested, nullptr), "observed command wrapper");
        CommandList cycle;
        cycle.commands.push_back(std::make_shared<RunLinkedCommandList>(&cycle));
        Check(!MigotoPort::ListsReadShaderConditions(&cycle, nullptr), "shader-free cycle terminates");
        cycle.commands.push_back(std::make_shared<RunLinkedCommandList>(&root));
        Check(MigotoPort::ListsReadShaderConditions(&cycle, nullptr), "shader reachable beyond cycle");
        Check(!MigotoPort::ListsReadShaderConditions(nullptr, nullptr), "empty lists");
        Check(argc == 2, "INI fixture directory argument");
        migoto_handle = GetModuleHandleW(nullptr);
        Check(PortApi::Load(argv[1]), "load INI fixtures");
        const wchar_t* dependent[] = { L"TextureOverrideRoverShapeKeyCallback", L"commandlistupdatemergedskeleton",
            L"commandlistrovernested", L"commandlistroverpost" };
        for (const auto* section : dependent) {
            Check(PortApi::HasSection(section), "dependent section parsed");
            Check(PortApi::SectionReadsShaderConditions(section), "INI section shader dependency");
        }
        const wchar_t* independent[] = { L"commandlistrovertexturefilter", L"commandlistroverenabled" };
        for (const auto* section : independent) {
            Check(PortApi::HasSection(section), "independent section parsed");
            Check(!PortApi::SectionReadsShaderConditions(section), "INI section false positive");
        }
        Check(!PortApi::SectionReadsShaderConditions(L"CommandListUnavailableCostume"), "missing section");
        std::puts("PASS: 18 XXMI expressions, nested/linked/post lists, wrappers, cycles and 6 parsed INI sections");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
