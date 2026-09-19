#include "ModelWorkflowCore.h"
#include <iostream>
#include <stdexcept>
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main() {
    try {
        using Core = ModelWorkflowCore;
        Core::Input input;
        auto state = Core::Evaluate(input);
        require(!state.canCapture && state.current == 0, "Missing bone allowed capture");
        for (int j = 0; j < 2; ++j) {
            input.jaw = j;
            auto& jaw = input.jaws[j];
            jaw.bone = true;
            require(Core::Evaluate(input).current == j * 4, "Wrong import step");
            jaw.dental = true;
            state = Core::Evaluate(input);
            require(state.current == j * 4 + 1 && state.canCapture && !state.canRegister, "Wrong points stage");
            jaw.bonePoints = 4; jaw.dentalPoints = 3;
            state = Core::Evaluate(input);
            require(!state.canRegister && state.nextTarget == Core::PointTarget::Dental, "Unmatched fourth point accepted");
            jaw.dentalPoints = 4;
            state = Core::Evaluate(input);
            require(state.canRegister && state.current == j * 4 + 2, "Complete pairs not registrable");
            jaw.registered = true;
            input.adjusting = true;
            state = Core::Evaluate(input);
            require(state.canBuild && !state.canCapture && state.done[j * 4 + 2] &&
                        state.current == j * 4 + 3,
                    "Active gizmo did not allow direct composite creation");
            require(state.instruction.contains(QStringLiteral("Crear modelo compuesto")),
                    "Active gizmo still asks for a separate acceptance");
            input.adjusting = false;
            require(Core::Evaluate(input).canBuild, "Registered jaw cannot build");
            for (auto phase : {Core::Phase::Block, Core::Phase::Computing, Core::Phase::Review}) {
                input.phase = phase;
                state = Core::Evaluate(input);
                require(!state.done[j * 4 + 3] && !state.canCapture && !state.canBuild, "Review skipped or tools active during block");
            }
            input.phase = Core::Phase::Registration;
            jaw.registered = false; // Rejected registration restores earlier state.
            require(Core::Evaluate(input).canRegister, "Rejected registration cannot be retried");
            jaw.accepted = true;
        }
        state = Core::Evaluate(input);
        require(state.current == -1, "Accepted composites not complete");
        for (bool done : state.done) require(done, "Reopened accepted composite lost completion");
        std::cout << "Model workflow OK\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
