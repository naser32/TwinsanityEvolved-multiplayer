#include "game/coop.h"

#include "game/agents.h"
#include "game/chunkloading.h"
#include "game/controllers.h"
#include "game/followcamera.h"
#include "game/gamecontroller.h"
#include "game/instancefactory.h"
#include "game/instances.h"
#include "game/math.h"
#include "game/objectnode.h"
#include "game/pads.h"
#include "game/place.h"
#include "game/player.h"
#include "game/progress.h"
#include "game/reference.h"
#include "game/resources.h"

// TwinsanityEvolved: the two-player mod (game/coop.h)

extern "C"
{
    // The chunk manager the instances' chunks are asked of (game/src/game/commandscharacters.cpp's)
    extern void* G_ChunkManager;
}

namespace
{

// The second controller (the pad controller's second pad: src/main.cpp makes two)
constexpr u32 SecondPadIndex = 1;
// Frames of play before the second player is made (a level's start settles first), and after its death
constexpr u32 SettleFrames = 30;
constexpr u32 DeathFrames = 90;
// Frames SELECT is held to leave
constexpr u32 LeaveFrames = 60;
// Where the second player is made: beside the first and a little above it (it drops to the ground)
constexpr f32 SpawnSide = 0.8f;
constexpr f32 SpawnAbove = 1.0f;
// The second player is brought back beside the first past this distance, or this far below it
constexpr f32 TetherDistance = 20.0f;
constexpr f32 TetherBelow = 12.0f;

// The second player's character (a reference: the game lets it go with its chunk)
Reference* g_Second = nullptr;
// Whether the second player has joined (START), and the frames left before it is made
bool g_Joined = false;
u32 g_Wait = 0;
u32 g_Selecting = 0;
bool g_Making = false;

GamePad* SecondPad()
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    return pads != nullptr ? pads->pads[SecondPadIndex] : nullptr;
}

InstanceContext* Second()
{
    return g_Second != nullptr ? static_cast<InstanceContext*>(g_Second->object) : nullptr;
}

CharacterAgent* CharacterOf(InstanceContext* instance)
{
    auto* node = instance != nullptr ? static_cast<PlayerNode*>(GetGameNode(&instance->nodes, NodeCharacter)) : nullptr;
    return node != nullptr ? node->character : nullptr;
}

Vector4 PositionOf(InstanceContext* instance)
{
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    return place->position;
}

// Beside the first player: its right hand side, above it
Vector4 BesideFirst(InstanceContext* first)
{
    Vector4 position = PositionOf(first);
    ObjectPlace* place = first->place;
    const f32 (*m)[4] = place->matrix.m;
    position.x = position.x + m[0][0] * SpawnSide;
    position.y = position.y + m[0][1] * SpawnSide + SpawnAbove;
    position.z = position.z + m[0][2] * SpawnSide;
    position.w = 1.0f;
    return position;
}

// Warped as the tests warp the played character (game/src/game/testrequests.cpp Warp: PositionWarp's WarpTo)
void Warp(InstanceContext* instance, const Vector4& position)
{
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    if (place->MoveTo(&position))
    {
        QueueObject(instance);
    }

    auto* node = static_cast<ObjectNode*>(GetGameNode(&instance->nodes, NodeObject));
    if (node != nullptr)
    {
        node->frameStart = position;
    }
}

// The second player's character let go of (released: gone from its chunk)
void TakeAway()
{
    InstanceContext* second = Second();
    if (second != nullptr)
    {
        auto* controls = static_cast<ControlsNode*>(GetGameNode(&second->nodes, NodeControls));
        if (controls != nullptr)
        {
            controls->pad = nullptr;
        }

        CallVirtual<u32>(second, second->vtable, ReferencedObject::ReleaseSlot);
    }

    RemoveReference(&g_Second);
    g_Second = nullptr;
}

// A copy of the first player's character made beside it (as a script's SpawnObject makes a copy of its own object: the source's
// properties, its object), played with the second controller; its follow camera stopped (the view stays the first player's)
InstanceContext* Make(InstanceContext* first)
{
    CharacterAgent* character = CharacterOf(first);
    ChunkEntry* chunk = ChunkOfInstance(G_ChunkManager, first);
    if (character == nullptr || chunk == nullptr)
    {
        return nullptr;
    }

    u32 objectId = character->objectId & ResourceIndexMask;
    Vector4 position = BesideFirst(first);
    s32 angles[3] = {0, 0, 0};
    ObjectPlace* place = first->place;
    place->SyncRotation();
    AnglesOfRotation(&place->rotation, &angles[0], &angles[1], &angles[2]);

    InstanceFactory* factory = g_InstanceFactory;
    factory->SetInstanceProperties();
    factory->ClearUnused1();
    factory->SetGivesIds();
    factory->ClearGivesFlagSlots();
    factory->creationFlags = 0;
    g_Making = true;
    InstanceContext* made = CreateInstanceFrom(factory, chunk, first, objectId, &position, angles, 0);
    g_Making = false;
    if (made == nullptr || made == first || CharacterOf(made) == nullptr)
    {
        return nullptr;
    }

    auto* controls = static_cast<ControlsNode*>(GetGameNode(&made->nodes, NodeControls));
    if (controls != nullptr)
    {
        controls->pad = SecondPad();
    }

    auto* follow = static_cast<FollowNode*>(GetGameNode(&made->nodes, NodeFollow));
    if (follow != nullptr)
    {
        follow->StopFollowing();
    }

    AssignReference(&g_Second, made);
    return made;
}

// Whether the first player's character can have a second beside it now: in play, on foot
InstanceContext* FirstInPlay(GameController* controller)
{
    if (controller->State() != GameController::StatePlaying)
    {
        return nullptr;
    }

    InstanceContext* first = controller->progress.Instance(controller->progress.play.character);
    CharacterAgent* character = CharacterOf(first);
    if (character == nullptr || first->chunk == nullptr || character->vehicle != nullptr || character->state.dead != 0)
    {
        return nullptr;
    }

    return first;
}

}

void StepCoop(GameController* controller)
{
    GamePad* pad = SecondPad();
    if (pad == nullptr)
    {
        return;
    }

    InstanceContext* first = FirstInPlay(controller);
    if (!g_Joined)
    {
        if (first != nullptr && GetButtonState(pad, PadStart, true))
        {
            g_Joined = true;
            g_Wait = 0;
        }

        return;
    }

    // Leaving: SELECT held
    g_Selecting = GetButtonState(pad, PadSelect, false) ? g_Selecting + 1 : 0;
    if (g_Selecting >= LeaveFrames)
    {
        g_Joined = false;
        g_Selecting = 0;
        TakeAway();
        return;
    }

    InstanceContext* second = Second();
    if (first == nullptr)
    {
        // Out of play (a cutscene, a vehicle, a load, the menus): taken away, made again once play goes on
        if (second != nullptr && controller->State() != GameController::StatePaused)
        {
            TakeAway();
        }

        if (controller->State() != GameController::StatePaused && g_Wait < SettleFrames)
        {
            g_Wait = SettleFrames;
        }

        return;
    }

    if (second == nullptr)
    {
        if (g_Wait > 0)
        {
            g_Wait--;
            return;
        }

        Make(first);
        return;
    }

    CharacterAgent* character = CharacterOf(second);
    if (character == nullptr)
    {
        TakeAway();
        return;
    }

    if (character->state.dead != 0)
    {
        // Its death plays out, then it's made again beside the first
        CoopSecondPlayerDied();
        return;
    }

    // Too far behind, or fallen below: brought back beside the first (made again when it's in another chunk)
    Vector4 a = PositionOf(first);
    Vector4 b = PositionOf(second);
    f32 dx = b.x - a.x;
    f32 dy = b.y - a.y;
    f32 dz = b.z - a.z;
    if (dx * dx + dy * dy + dz * dz > TetherDistance * TetherDistance || dy < -TetherBelow)
    {
        if (second->chunk == first->chunk)
        {
            Warp(second, BesideFirst(first));
        }
        else
        {
            TakeAway();
        }
    }
}

bool CoopIsSecondPlayer(InstanceContext* instance)
{
    return instance != nullptr && instance == Second();
}

void CoopSecondPlayerDied()
{
    TakeAway();
    g_Wait = DeathFrames;
}

bool CoopMakingSecondPlayer()
{
    return g_Making;
}
