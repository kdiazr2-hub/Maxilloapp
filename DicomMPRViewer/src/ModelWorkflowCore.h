#pragma once
#include <array>
#include <QString>

class ModelWorkflowCore {
public:
    enum class Phase { Registration, Block, Computing, Review };
    enum class PointTarget { None, Bone, Dental };
    struct Jaw {
        bool bone = false, dental = false, registered = false, accepted = false;
        int bonePoints = 0, dentalPoints = 0;
    };
    struct Input {
        std::array<Jaw, 2> jaws;
        int jaw = 0;
        Phase phase = Phase::Registration;
        PointTarget capture = PointTarget::None;
        bool adjusting = false;
    };
    struct State {
        std::array<bool, 8> done{};
        int current = 0;
        bool canCapture = false, canRegister = false, canAdjust = false, canBuild = false;
        PointTarget nextTarget = PointTarget::None;
        QString instruction, points;
    };
    static State Evaluate(const Input& input);
};
