/*
 * Copyright (C) 2005-2013 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "MoveSplineInit.h"
#include "MoveSpline.h"
#include "packet_builder.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "Unit.h"
#include "Transport.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Anticheat.h"
#include "MovementPacketSender.h"

namespace Movement
{
UnitMoveType SelectSpeedType(uint32 moveFlags)
{
    if (moveFlags & MOVEFLAG_SWIMMING)
    {
        if (moveFlags & MOVEFLAG_BACKWARD /*&& speed_obj.swim >= speed_obj.swim_back*/)
            return MOVE_SWIM_BACK;
        else
            return MOVE_SWIM;
    }
    else if (moveFlags & MOVEFLAG_WALK_MODE)
    {
        // if (speed_obj.run > speed_obj.walk)
        return MOVE_WALK;
    }
    else if (moveFlags & MOVEFLAG_BACKWARD /*&& speed_obj.run >= speed_obj.run_back*/)
        return MOVE_RUN_BACK;

    return MOVE_RUN;
}


void MoveSplineInit::Move(PathFinder const* pfinder)
{
    MovebyPath(pfinder->getPath());
    if (pfinder->GetTransport())
        SetTransport(pfinder->GetTransport()->GetGUIDLow());
    if (pfinder->getPathType() & PATHFIND_FLYPATH)
        SetFly();
}

static thread_local uint32 splineCounter = 1;

int32 MoveSplineInit::Launch()
{
    MoveSpline& move_spline = *unit.movespline;

    GenericTransport* newTransport = nullptr;
    if (args.transportGuid)
        newTransport = unit.GetMap()->GetTransport(sObjectMgr.GetFullTransportGuidFromLowGuid(args.transportGuid));

    Vector3 real_position(unit.GetPositionX(), unit.GetPositionY(), unit.GetPositionZ());

    // there is a big chance that current position is unknown if current state is not finalized, need compute it
    // this also allows calculate spline position and update map position in much greater intervals
    if (!move_spline.Finalized())
    {
        real_position = move_spline.ComputePosition();
        GenericTransport* oldTransport = nullptr;
        if (move_spline.GetTransportGuid())
            oldTransport = unit.GetMap()->GetTransport(sObjectMgr.GetFullTransportGuidFromLowGuid(move_spline.GetTransportGuid()));
        if (oldTransport)
            oldTransport->CalculatePassengerPosition(real_position.x, real_position.y, real_position.z);
    }

    if (newTransport)
        newTransport->CalculatePassengerOffset(real_position.x, real_position.y, real_position.z, &args.facing.angle);

    if (args.path.empty())
    {
        // should i do the things that user should do?
        MoveTo(real_position);
    }

    // corrent first vertex
    args.path[0] = real_position;
    uint32 moveFlags = unit.m_movementInfo.GetMovementFlags();
    uint32 oldMoveFlags = moveFlags;
    if (args.flags.done)
    {
        args.flags = MoveSplineFlag::Done;
        moveFlags &= ~(MOVEFLAG_SPLINE_ENABLED | MOVEFLAG_MASK_MOVING);
    }
    else
    {
        moveFlags |= (MOVEFLAG_SPLINE_ENABLED | MOVEFLAG_FORWARD);

        if (args.flags.runmode)
            moveFlags &= ~MOVEFLAG_WALK_MODE;
        else
            moveFlags |= MOVEFLAG_WALK_MODE;
    }

    if (newTransport)
        moveFlags |= MOVEFLAG_ONTRANSPORT;
    else
        moveFlags &= ~MOVEFLAG_ONTRANSPORT;

    if (args.velocity == 0.f)
        args.velocity = unit.GetSpeed(SelectSpeedType(moveFlags));

    if (!args.Validate(&unit))
        return 0;

    args.splineId = splineCounter++;

    if (Player* pPlayer = unit.ToPlayer())
        pPlayer->GetCheatData()->ResetJumpCounters();

    if (unit.IsPlayer() || unit.GetPossessorGuid().IsPlayer())
        unit.SetSplineDonePending(true);

    unit.m_movementInfo.ctime = 0;
    unit.m_movementInfo.SetMovementFlags((MovementFlags)moveFlags);
    move_spline.SetMovementOrigin(movementType);
    move_spline.Initialize(args);
    
    WorldPacket data(SMSG_MONSTER_MOVE, 64);
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
    data << unit.GetPackGUID();
#else
    data << unit.GetGUID();
#endif

    if (newTransport)
    {
        data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
        data << newTransport->GetPackGUID();
#else
        data << newTransport->GetGUID();
#endif
    }

    if (unit.GetTransport() && unit.GetTransport() != newTransport)
        unit.GetTransport()->RemovePassenger(&unit);
    if (newTransport && unit.GetTransport() != newTransport)
        newTransport->AddPassenger(&unit);

    // Stop packet
    if (args.flags.done)
    {
        data << real_position.x << real_position.y << real_position.z;
        data << move_spline.GetId();
        data << uint8(1); // MonsterMoveStop=1
    }
    else
        move_spline.setLastPointSent(PacketBuilder::WriteMonsterMove(move_spline, data));

    if ((oldMoveFlags & MOVEFLAG_ROOT) && !args.flags.done)
        MovementPacketSender::SendMovementFlagChangeToAll(&unit, MOVEFLAG_ROOT, false);
    if (oldMoveFlags & MOVEFLAG_WALK_MODE && !(moveFlags & MOVEFLAG_WALK_MODE)) // Switch to run mode
        MovementPacketSender::SendToggleRunWalkToAll(&unit, true);
    if (moveFlags & MOVEFLAG_WALK_MODE && !(oldMoveFlags & MOVEFLAG_WALK_MODE)) // Switch to walk mode
        MovementPacketSender::SendToggleRunWalkToAll(&unit, false);
        
    unit.SendMovementMessageToSet(std::move(data), true);

    // Restore correct walk mode for players
    if (unit.GetTypeId() == TYPEID_PLAYER && (moveFlags & MOVEFLAG_WALK_MODE) != (oldMoveFlags & MOVEFLAG_WALK_MODE))
        MovementPacketSender::SendToggleRunWalkToAll(&unit, !(oldMoveFlags & MOVEFLAG_WALK_MODE));
    
    return move_spline.Duration();
}

MoveSplineInit::MoveSplineInit(Unit& m, char const* mvtType) : unit(m), movementType(mvtType)
{
    // mix existing state into new
    args.flags.runmode = !unit.m_movementInfo.HasMovementFlag(MOVEFLAG_WALK_MODE);
    args.flags.flying = unit.m_movementInfo.HasMovementFlag((MovementFlags)(MOVEFLAG_FLYING | MOVEFLAG_LEVITATING));
}

void MoveSplineInit::SetFacingGUID(uint64 guid)
{
    args.flags.EnableFacingTarget();
    args.facing.target = guid;
}

void MoveSplineInit::SetFacing(float angle)
{
    args.facing.angle = G3D::wrap(angle, 0.f, (float)G3D::twoPi());
    args.flags.EnableFacingAngle();
}
}
