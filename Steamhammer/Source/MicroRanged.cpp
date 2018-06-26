#include "MicroRanged.h"
#include "CombatCommander.h"
#include "UnitUtil.h"
#include "BuildingPlacer.h"

const double pi = 3.14159265358979323846;

namespace { auto & bwebMap = BWEB::Map::Instance(); }

using namespace UAlbertaBot;

// The unit's ranged ground weapon does splash damage, so it works under dark swarm.
// Firebats are not here: They are melee units.
// Tanks and lurkers are not here: They have their own micro managers.
bool MicroRanged::goodUnderDarkSwarm(BWAPI::UnitType type)
{
	return
		type == BWAPI::UnitTypes::Protoss_Archon ||
		type == BWAPI::UnitTypes::Protoss_Reaver;
}

// -----------------------------------------------------------------------------------------

MicroRanged::MicroRanged()
{ 
}

void MicroRanged::getTargets(BWAPI::Unitset & targets) const
{
	if (order.getType() != SquadOrderTypes::HoldWall)
	{
		MicroManager::getTargets(targets);
		return;
	}

	LocutusWall& wall = BuildingPlacer::Instance().getWall();

	for (const auto unit : BWAPI::Broodwar->enemy()->getUnits())
	{
		if (unit->exists() &&
			(unit->isCompleted() || unit->getType().isBuilding()) &&
			unit->getHitPoints() > 0 &&
			unit->getType() != BWAPI::UnitTypes::Unknown
			&& (wall.tilesInsideWall.find(unit->getTilePosition()) != wall.tilesInsideWall.end() 
				|| wall.tilesOutsideWall.find(unit->getTilePosition()) != wall.tilesOutsideWall.end()))
		{
			targets.insert(unit);
		}
	}
}

void MicroRanged::executeMicro(const BWAPI::Unitset & targets) 
{
	assignTargets(targets);
}

struct CompareTiles {
	bool operator() (const std::pair<BWAPI::TilePosition, double>& lhs, const std::pair<BWAPI::TilePosition, double>& rhs) const {
		return lhs.second < rhs.second;
	}
};

struct CompareUnits {
	bool operator() (const std::pair<const BWAPI::Unit*, double>& lhs, const std::pair<const BWAPI::Unit*, double>& rhs) const {
		return lhs.second < rhs.second;
	}
};

BWAPI::Position center(BWAPI::TilePosition tile)
{
	return BWAPI::Position(tile) + BWAPI::Position(16, 16);
}

void MicroRanged::assignTargets(const BWAPI::Unitset & targets)
{
    const BWAPI::Unitset & rangedUnits = getUnits();
    Squad & squad = CombatCommander::Instance().getSquadData().getSquad(this);

	// The set of potential targets.
	BWAPI::Unitset rangedUnitTargets;
    std::copy_if(targets.begin(), targets.end(), std::inserter(rangedUnitTargets, rangedUnitTargets.end()),
		[](BWAPI::Unit u) {
		return
			u->isVisible() &&
			u->isDetected() &&
			u->getType() != BWAPI::UnitTypes::Zerg_Larva &&
			u->getType() != BWAPI::UnitTypes::Zerg_Egg &&
			!u->isStasised();
	});

	// Special case for moving units when we are holding the wall and there are no targets
	if (order.getType() == SquadOrderTypes::HoldWall && targets.empty())
	{
		LocutusWall & wall = BuildingPlacer::Instance().getWall();

		// Populate the set of available tiles inside the wall
		std::set<std::pair<BWAPI::TilePosition, double>, CompareTiles> availableTilesInside;
		for (const auto& tile : wall.tilesInsideWall)
			if (!BWEB::Map::Instance().overlapsAnything(tile))
			{
				double dist = center(tile).getDistance(wall.gapCenter);
				if (dist < 23) continue; // Don't include the door tile itself
				availableTilesInside.insert(std::make_pair(tile, dist));
			}

		// Populate the set of available tiles outside the wall
		std::set<std::pair<BWAPI::TilePosition, double>, CompareTiles> availableTilesOutside;
		for (const auto& tile : wall.tilesOutsideWall)
			if (!BWEB::Map::Instance().overlapsAnything(tile))
			{
				double dist = center(tile).getDistance(wall.gapCenter);
				if (dist < 23) continue; // Don't include the door tile itself
				availableTilesOutside.insert(std::make_pair(tile, dist));
			}

		// Remove the occupied tiles and populate unit sets
		std::set<std::pair<BWAPI::Unit, double>> insideUnitsByDistanceToDoor;
		std::set<std::pair<BWAPI::Unit, double>> outsideUnitsByDistanceToDoor;
		BWAPI::Position closestTileInside = center(availableTilesInside.begin()->first);
		BWAPI::Position closestTileOutside = center(availableTilesInside.begin()->first);
		for (const auto & rangedUnit : rangedUnits)
		{
			for (auto it = availableTilesInside.begin(); it != availableTilesInside.end(); )
				if (it->first == rangedUnit->getTilePosition())
				{
					availableTilesInside.erase(it);
					break;
				}
				else
					it++;

			for (auto it = availableTilesOutside.begin(); it != availableTilesOutside.end(); )
				if (it->first == rangedUnit->getTilePosition())
				{
					availableTilesOutside.erase(it);
					break;
				}
				else
					it++;

			double dist = rangedUnit->getPosition().getDistance(wall.gapCenter);
			if (rangedUnit->getPosition().getDistance(closestTileInside) < rangedUnit->getPosition().getDistance(closestTileOutside))
				insideUnitsByDistanceToDoor.insert(std::make_pair(rangedUnit, dist));
			else
				outsideUnitsByDistanceToDoor.insert(std::make_pair(rangedUnit, dist));
		}

		// Issue orders to units in order of their distance to the door
		for (const auto & unit : insideUnitsByDistanceToDoor)
		{
			// Is the first free tile closer than this one?
			if (availableTilesInside.begin()->second < (unit.second - 16))
			{
				// Move to the free tile
				Micro::Move(unit.first, center(availableTilesInside.begin()->first));

				// Remove the tile from the available set
				availableTilesInside.erase(availableTilesInside.begin());

				// Add our former tile to the available set
				availableTilesInside.insert(std::make_pair(unit.first->getTilePosition(), unit.second));
			}

			// The tile wasn't closer, so stop
			else
				Micro::Stop(unit.first);
		}

		for (const auto & unit : outsideUnitsByDistanceToDoor)
		{
			// Is the first free tile closer than this one?
			if (availableTilesOutside.begin()->second < (unit.second - 16))
			{
				// Move to the free tile
				Micro::Move(unit.first, center(availableTilesOutside.begin()->first));

				// Remove the tile from the available set
				availableTilesOutside.erase(availableTilesOutside.begin());

				// Add our former tile to the available set
				availableTilesOutside.insert(std::make_pair(unit.first->getTilePosition(), unit.second));
			}

			// The tile wasn't closer, so stop
			else
				Micro::Stop(unit.first);
		}

		return;
	}

    for (const auto rangedUnit : rangedUnits)
	{
		if (buildScarabOrInterceptor(rangedUnit))
		{
			// If we started one, no further action this frame.
			continue;
		}

		if (rangedUnit->isBurrowed())
		{
			// For now, it would burrow only if irradiated. Leave it.
			// Lurkers are controlled by a different class.
			continue;
		}

		// Special case for irradiated zerg units.
		if (rangedUnit->isIrradiated() && rangedUnit->getType().getRace() == BWAPI::Races::Zerg)
		{
			if (rangedUnit->isFlying())
			{
				if (rangedUnit->getDistance(order.getPosition()) < 300)
				{
					Micro::AttackMove(rangedUnit, order.getPosition());
				}
				else
				{
					Micro::Move(rangedUnit, order.getPosition());
				}
				continue;
			}
			else if (rangedUnit->canBurrow())
			{
				rangedUnit->burrow();
				continue;
			}
		}

		// Carriers stay at home until they have enough interceptors to be useful,
		// or retreat toward home to rebuild them if they run low.
		// On attack-move so that they're not helpless, but that can cause problems too....
		// Potentially useful for other units.
		// NOTE Regrouping can cause the carriers to move away from home.
		if (stayHomeUntilReady(rangedUnit))
		{
			BWAPI::Position fleeTo(InformationManager::Instance().getMyMainBaseLocation()->getPosition());
			Micro::AttackMove(rangedUnit, fleeTo);
			continue;
		}

		if (order.isCombatOrder())
        {
			if (unstickStuckUnit(rangedUnit))
			{
				continue;
			}

			// If a target is found,
			BWAPI::Unit target = getTarget(rangedUnit, rangedUnitTargets);
			if (target)
			{
				if (Config::Debug::DrawUnitTargetInfo)
				{
					BWAPI::Broodwar->drawLineMap(rangedUnit->getPosition(), rangedUnit->getTargetPosition(), BWAPI::Colors::Purple);
				}

				// attack it.
                // Bunkers are handled by a special micro manager, unless we have a large army
                if (rangedUnits.size() < 6 &&
                    target->getType() == BWAPI::UnitTypes::Terran_Bunker &&
                    target->isCompleted())
                {
                    squad.addUnitToBunkerAttackSquad(target, rangedUnit);
                }
				else if (Config::Micro::KiteWithRangedUnits)
				{
					kite(rangedUnit, target);
				}
				else
				{
					Micro::AttackUnit(rangedUnit, target);
				}
			}
			else
			{
                // No target found. If we're not near the order position, go there.
				if (rangedUnit->getDistance(order.getPosition()) > 100)
				{
                    // If this unit is doing a bunker run-by, get the position it should move towards
                    auto bunkerRunBySquad = squad.getBunkerRunBySquad(rangedUnit);
                    if (bunkerRunBySquad)
                    {
                        InformationManager::Instance().getLocutusUnit(rangedUnit)
                            .moveTo(bunkerRunBySquad->getRunByPosition(rangedUnit, order.getPosition()));
                        //Micro::Move(rangedUnit, bunkerRunBySquad->getRunByPosition(rangedUnit, order.getPosition()));
                    }
                    else
                    {
                        InformationManager::Instance().getLocutusUnit(rangedUnit).moveTo(order.getPosition());
                        //Micro::Move(rangedUnit, order.getPosition());
                    }
				}
			}
		}
	}
}

// This can return null if no target is worth attacking.
BWAPI::Unit MicroRanged::getTarget(BWAPI::Unit rangedUnit, const BWAPI::Unitset & targets)
{
	int bestScore = -999999;
	BWAPI::Unit bestTarget = nullptr;
	int bestPriority = -1;   // TODO debug only

	for (const auto target : targets)
	{
		// Skip targets under dark swarm that we can't hit.
		if (target->isUnderDarkSwarm() && !goodUnderDarkSwarm(rangedUnit->getType()))
		{
			continue;
		}

		const int priority = getAttackPriority(rangedUnit, target);		// 0..12
		const int range = rangedUnit->getDistance(target);				// 0..map diameter in pixels
		const int closerToGoal =										// positive if target is closer than us to the goal
			rangedUnit->getDistance(order.getPosition()) - target->getDistance(order.getPosition());
		
		// Skip targets that are too far away to worry about--outside tank range.
		if (range >= 13 * 32)
		{
			continue;
		}

		// Let's say that 1 priority step is worth 160 pixels (5 tiles).
		// We care about unit-target range and target-order position distance.
		int score = 5 * 32 * priority - range;

		// Adjust for special features.
		// A bonus for attacking enemies that are "in front".
		// It helps reduce distractions from moving toward the goal, the order position.
		if (closerToGoal > 0)
		{
			score += 2 * 32;
		}

		const bool isThreat = UnitUtil::CanAttack(target, rangedUnit);   // may include workers as threats
		const bool canShootBack = isThreat && target->isInWeaponRange(rangedUnit);

		if (isThreat)
		{
			if (canShootBack)
			{
				score += 6 * 32;
			}
			else if (rangedUnit->isInWeaponRange(target))
			{
				score += 4 * 32;
			}
			else
			{
				score += 3 * 32;
			}
		}
		// This could adjust for relative speed and direction, so that we don't chase what we can't catch.
		else if (!target->isMoving())
		{
			if (target->isSieged() ||
				target->getOrder() == BWAPI::Orders::Sieging ||
				target->getOrder() == BWAPI::Orders::Unsieging)
			{
				score += 48;
			}
			else
			{
				score += 24;
			}
		}
		else if (target->isBraking())
		{
			score += 16;
		}
		else if (target->getType().topSpeed() >= rangedUnit->getType().topSpeed())
		{
			score -= 4 * 32;
		}
		
		// Prefer targets that are already hurt.
		if (target->getType().getRace() == BWAPI::Races::Protoss && target->getShields() <= 5)
		{
			score += 32;
		}
		if (target->getHitPoints() < target->getType().maxHitPoints())
		{
			score += 24;
		}

		// Prefer to hit air units that have acid spores on them from devourers.
		if (target->getAcidSporeCount() > 0)
		{
			// Especially if we're a mutalisk with a bounce attack.
			if (rangedUnit->getType() == BWAPI::UnitTypes::Zerg_Mutalisk)
			{
				score += 16 * target->getAcidSporeCount();
			}
			else
			{
				score += 8 * target->getAcidSporeCount();
			}
		}

		// Take the damage type into account.
		BWAPI::DamageType damage = UnitUtil::GetWeapon(rangedUnit, target).damageType();
		if (damage == BWAPI::DamageTypes::Explosive)
		{
			if (target->getType().size() == BWAPI::UnitSizeTypes::Large)
			{
				score += 32;
			}
		}
		else if (damage == BWAPI::DamageTypes::Concussive)
		{
			if (target->getType().size() == BWAPI::UnitSizeTypes::Small)
			{
				score += 32;
			}
			else if (target->getType().size() == BWAPI::UnitSizeTypes::Large)
			{
				score -= 32;
			}
		}

		if (score > bestScore)
		{
			bestScore = score;
			bestTarget = target;

			bestPriority = priority;
		}
	}

	return bestScore > 0 && !shouldIgnoreTarget(rangedUnit, bestTarget) ? bestTarget : nullptr;
}

// get the attack priority of a target unit
int MicroRanged::getAttackPriority(BWAPI::Unit rangedUnit, BWAPI::Unit target) 
{
	const BWAPI::UnitType rangedType = rangedUnit->getType();
	const BWAPI::UnitType targetType = target->getType();

	if (rangedType == BWAPI::UnitTypes::Zerg_Scourge)
    {
		if (!targetType.isFlyer())
		{
			// Can't target it. Also, ignore lifted buildings.
			return 0;
		}
		if (targetType == BWAPI::UnitTypes::Zerg_Overlord ||
			targetType == BWAPI::UnitTypes::Zerg_Scourge ||
			targetType == BWAPI::UnitTypes::Protoss_Interceptor)
		{
			// Usually not worth scourge at all.
			return 0;
		}
		
		// Arbiters first.
		if (targetType == BWAPI::UnitTypes::Protoss_Arbiter)
		{
			return 10;
		}

		// Everything else is the same. Hit whatever's closest.
		return 9;
	}

	if (rangedType == BWAPI::UnitTypes::Zerg_Guardian && target->isFlying())
	{
		// Can't target it.
		return 0;
	}

	// A carrier should not target an enemy interceptor.
	if (rangedType == BWAPI::UnitTypes::Protoss_Carrier && targetType == BWAPI::UnitTypes::Protoss_Interceptor)
	{
		return 0;
	}

	// An addon other than a completed comsat is boring.
	// TODO should also check that it is attached
	if (targetType.isAddon() && !(targetType == BWAPI::UnitTypes::Terran_Comsat_Station && target->isCompleted()))
	{
		return 1;
	}

    // if the target is building something near our base something is fishy
    BWAPI::Position ourBasePosition = BWAPI::Position(InformationManager::Instance().getMyMainBaseLocation()->getPosition());
	if (target->getDistance(ourBasePosition) < 1000) {
		if (target->getType().isWorker() && (target->isConstructing() || target->isRepairing()))
		{
			return 12;
		}
		if (target->getType().isBuilding())
		{
			// This includes proxy buildings, which deserve high priority.
			// But when bases are close together, it can include innocent buildings.
			// We also don't want to disrupt priorities in case of proxy buildings
			// supported by units; we may want to target the units first.
			if (UnitUtil::CanAttackGround(target) || UnitUtil::CanAttackAir(target))
			{
				return 10;
			}
			return 8;
		}
	}
    
	if (rangedType.isFlyer()) {
		// Exceptions if we're a flyer (other than scourge, which is handled above).
		if (targetType == BWAPI::UnitTypes::Zerg_Scourge)
		{
			return 12;
		}
	}
	else
	{
		// Exceptions if we're a ground unit.
		if (targetType == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine && !target->isBurrowed() ||
			targetType == BWAPI::UnitTypes::Zerg_Infested_Terran)
		{
			return 12;
		}
	}

	// Wraiths, scouts, and goliaths strongly prefer air targets because they do more damage to air units.
	if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Wraith ||
		rangedUnit->getType() == BWAPI::UnitTypes::Protoss_Scout)
	{
		if (target->getType().isFlyer())    // air units, not floating buildings
		{
			return 11;
		}
	}
	else if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Goliath)
	{
		if (target->getType().isFlyer())    // air units, not floating buildings
		{
			return 10;
		}
	}

	if (targetType == BWAPI::UnitTypes::Protoss_High_Templar ||
		targetType == BWAPI::UnitTypes::Protoss_Reaver)
	{
		return 12;
	}

	if (targetType == BWAPI::UnitTypes::Protoss_Arbiter)
	{
		return 11;
	}

	if (targetType == BWAPI::UnitTypes::Terran_Bunker)
	{
		return 9;
	}

	// Threats can attack us. Exceptions: Workers are not threats.
	if (UnitUtil::CanAttack(targetType, rangedType) && !targetType.isWorker())
	{
		// Enemy unit which is far enough outside its range is lower priority than a worker.
		if (rangedUnit->getDistance(target) > 48 + UnitUtil::GetAttackRange(target, rangedUnit))
		{
			return 8;
		}
		return 10;
	}
	// Droppers are as bad as threats. They may be loaded and are often isolated and safer to attack.
	if (targetType == BWAPI::UnitTypes::Terran_Dropship ||
		targetType == BWAPI::UnitTypes::Protoss_Shuttle)
	{
		return 10;
	}
	// Also as bad are other dangerous things.
	if (targetType == BWAPI::UnitTypes::Terran_Science_Vessel ||
		targetType == BWAPI::UnitTypes::Zerg_Scourge ||
		targetType == BWAPI::UnitTypes::Protoss_Observer)
	{
		return 10;
	}
	// Next are workers.
	if (targetType.isWorker()) 
	{
        if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            return 11;
        }
		// Blocking a narrow choke makes you critical.
		if (unitNearNarrowChokepoint(target))
		{
			return 11;
		}
        // Repairing something that can shoot makes you critical.
        if (target->isRepairing() && target->getOrderTarget() && target->getOrderTarget()->getType().groundWeapon() != BWAPI::WeaponTypes::None)
        {
            return 11;
        }
        // SCVs so close to the unit that they are likely to be attacking it are important
        if (rangedUnit->getDistance(target) < 32)
        {
            return 10;
        }
        // SCVs constructing are also important.
        if (target->isConstructing())
        {
            return 9;
        }

  		return 8;
	}
    // Sieged tanks are slightly more important than unsieged tanks
    if (targetType == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
    {
        return 9;
    }
	// Important combat units that we may not have targeted above (esp. if we're a flyer).
	if (targetType == BWAPI::UnitTypes::Protoss_Carrier ||
		targetType == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
	{
		return 8;
	}
	// Nydus canal is the most important building to kill.
	if (targetType == BWAPI::UnitTypes::Zerg_Nydus_Canal)
	{
		return 10;
	}
	// Spellcasters are as important as key buildings.
	// Also remember to target other non-threat combat units.
	if (targetType.isSpellcaster() ||
		targetType.groundWeapon() != BWAPI::WeaponTypes::None ||
		targetType.airWeapon() != BWAPI::WeaponTypes::None)
	{
		return 7;
	}
	// Templar tech and spawning pool are more important.
	if (targetType == BWAPI::UnitTypes::Protoss_Templar_Archives)
	{
		return 7;
	}
	if (targetType == BWAPI::UnitTypes::Zerg_Spawning_Pool)
	{
		return 7;
	}
	// Don't forget the nexus/cc/hatchery.
	if (targetType.isResourceDepot())
	{
		return 6;
	}
	if (targetType == BWAPI::UnitTypes::Protoss_Pylon)
	{
		return 5;
	}
	if (targetType == BWAPI::UnitTypes::Terran_Factory || targetType == BWAPI::UnitTypes::Terran_Armory)
	{
		return 5;
	}
	// Downgrade unfinished/unpowered buildings, with exceptions.
	if (targetType.isBuilding() &&
		(!target->isCompleted() || !target->isPowered()) &&
		!(	targetType.isResourceDepot() ||
			targetType.groundWeapon() != BWAPI::WeaponTypes::None ||
			targetType.airWeapon() != BWAPI::WeaponTypes::None ||
			targetType == BWAPI::UnitTypes::Terran_Bunker))
	{
		return 2;
	}
	if (targetType.gasPrice() > 0)
	{
		return 4;
	}
	if (targetType.mineralPrice() > 0)
	{
		return 3;
	}
	// Finally everything else.
	return 1;
}

// Should the unit stay (or return) home until ready to move out?
bool MicroRanged::stayHomeUntilReady(const BWAPI::Unit u) const
{
	return
		u->getType() == BWAPI::UnitTypes::Protoss_Carrier && u->getInterceptorCount() < 4;
}

void MicroRanged::kite(BWAPI::Unit rangedUnit, BWAPI::Unit target)
{
    double range(rangedUnit->getType().groundWeapon().maxRange());
    if (rangedUnit->getType() == BWAPI::UnitTypes::Protoss_Dragoon && BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Singularity_Charge))
    {
        range = 6 * 32;
    }
    else if (rangedUnit->getType() == BWAPI::UnitTypes::Zerg_Hydralisk && BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Grooved_Spines))
    {
        range = 5 * 32;
    }

    // Check if we are blocking a ramp
    bool blockingRamp = false;
    for (BWTA::Chokepoint * choke : BWTA::getChokepoints())
    {
        if (choke->getWidth() < 64 &&
            rangedUnit->getDistance(choke->getCenter()) < 64)
        {
            // We're close to a choke, find out if there are friendly units behind us

            // Start by computing the angle of the choke
            BWAPI::Position chokeDelta(choke->getSides().first, choke->getSides().second);
            double chokeAngle = atan2(chokeDelta.y, chokeDelta.x);

            // Now find points ahead and behind us with respect to the choke
            // We'll find out which is which in a moment
            BWAPI::Position first(
                rangedUnit->getPosition().x - (int)std::round(64 * std::cos(chokeAngle + (pi / 2.0))),
                rangedUnit->getPosition().y - (int)std::round(64 * std::sin(chokeAngle + (pi / 2.0))));
            BWAPI::Position second(
                rangedUnit->getPosition().x - (int)std::round(64 * std::cos(chokeAngle - (pi / 2.0))),
                rangedUnit->getPosition().y - (int)std::round(64 * std::sin(chokeAngle - (pi / 2.0))));

            // Find out which position is behind us
            BWAPI::Position position = first;
            if (target->getDistance(second) > target->getDistance(first))
                position = second;

            // Now check how many friendly units are close to it
            int friendlies = 0;
            for (auto & unit : getUnits())
            {
                if (unit == rangedUnit) continue;
                if (unit->getDistance(position) < 64)
                {
                    friendlies++;
                    break;
                }
            }

            blockingRamp = friendlies >= 2;

            break;
        }
    }

    // Move towards the target if:
    // - it is a sieged tank
    // - we are blocking a ramp
    if (rangedUnit->getGroundWeaponCooldown() > 0 &&
        (blockingRamp ||
            target->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode))
    {
        if (rangedUnit->getDistance(target) > 48)
        {
            Micro::Move(rangedUnit, target->getPosition());
        }
        else
        {
            Micro::AttackUnit(rangedUnit, target);
        }

        return;
    }

    bool kite(true);

    // Don't kite if the enemy's range is at least as long as ours.
    // NOTE Assumes that the enemy does not have range upgrades, and only checks ground range.
    // Also, if the target can't attack back, then don't kite.
    if (range <= target->getType().groundWeapon().maxRange() ||
        !UnitUtil::CanAttack(target, rangedUnit))
    {
        kite = false;
    }

    // Kite if we're not ready yet: Wait for the weapon.
    double dist(rangedUnit->getDistance(target));
    double speed(rangedUnit->getType().topSpeed());
    double timeToEnter = 0.0;                      // time to reach firing range
    if (speed > .00001)                            // don't even visit the same city as division by zero
    {
        timeToEnter = std::max(0.0, dist - range) / speed;
    }
    if (timeToEnter >= rangedUnit->getGroundWeaponCooldown() ||
        target->getType().isBuilding())
    {
        kite = false;
    }

    // Don't kite if the enemy is moving away from us
    if (kite)
    {
        BWAPI::Position predictedPosition = InformationManager::Instance().predictUnitPosition(target, 1);
        if (predictedPosition.isValid() && rangedUnit->getDistance(predictedPosition) > rangedUnit->getDistance(target->getPosition()))
        {
            kite = false;
        }
    }

    if (!kite)
    {
        Micro::AttackUnit(rangedUnit, target);
        return;
    }

    // Determine the position to move towards
    // Criteria:
    // - Walkable and not blocked by a building
    // - TODO: Not blocking other friendly units from moving into firing position
    // - TODO: Generally moving us away from nearby enemy units
    // - TODO: Far from enemy static defense

    // Distance we want to move away from our current position
    double distance = 32.0;
    if (speed > 0.1) distance = rangedUnit->getGroundWeaponCooldown() * speed;

    // Our current angle relative to the target
    BWAPI::Position delta(target->getPosition() - rangedUnit->getPosition());
    double angleToTarget = atan2(delta.y, delta.x);

    // Score moving at a variety of angles
    double bestScore = DBL_MAX;
    BWAPI::Position bestPosition = BWAPI::Positions::Invalid;

    for (int i = -3; i <= 3; i++)
    {
        double a = angleToTarget + (i * pi / 6);

        BWAPI::Position position(
            rangedUnit->getPosition().x - (int)std::round(distance * std::cos(a)),
            rangedUnit->getPosition().y - (int)std::round(distance * std::sin(a)));

        // Valid and walkable
        if (!position.isValid() ||
            !BWAPI::WalkPosition(position).isValid() ||
            !BWAPI::Broodwar->isWalkable(BWAPI::WalkPosition(position)))
        {
            continue;
        }

        // Not blocked by a building
        if (bwebMap.usedTiles.find(BWAPI::TilePosition(position)) != bwebMap.usedTiles.end())
        {
            continue;
        }

        // Score
        // TODO: Add more metrics
        double score = std::abs(a - angleToTarget);

        if (score < bestScore)
        {
            bestScore = score;
            bestPosition = position;
        }
    }

    if (bestPosition.isValid())
    {
        Micro::Move(rangedUnit, bestPosition);
    }
}
