/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_gui.cpp %File for dealing with rail construction user interface. */

#include "stdafx.h"
#include "gui.h"
#include "station_base.h"
#include "waypoint_base.h"
#include "window_gui.h"
#include "station_gui.h"
#include "terraform_gui.h"
#include "viewport_func.h"
#include "command_func.h"
#include "waypoint_func.h"
#include "newgrf_badge.h"
#include "newgrf_badge_gui.h"
#include "newgrf_station.h"
#include "company_base.h"
#include "strings_func.h"
#include "window_func.h"
#include "sound_func.h"
#include "company_func.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "tunnelbridge.h"
#include "tilehighlight_func.h"
#include "core/geometry_func.hpp"
#include "hotkeys.h"
#include "engine_base.h"
#include "vehicle_func.h"
#include "zoom_func.h"
#include "rail_gui.h"
#include "toolbar_gui.h"
#include "landscape_cmd.h"
#include "station_cmd.h"
#include "tilearea_type.h"
#include "terraform_cmd.h"
#include "tunnelbridge_cmd.h"
#include "waypoint_cmd.h"
#include "rail_cmd.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "picker_gui.h"
#include "debug.h"

#include "station_map.h"
#include "tunnelbridge_map.h"
#include "water_map.h"

#include "widgets/rail_widget.h"

#include "table/strings.h"

#include "safeguards.h"

static RailType _cur_railtype;               ///< Rail type of the current build-rail toolbar.
static bool _remove_button_clicked;          ///< Flag whether 'remove' toggle-button is currently enabled
static DiagDirection _build_depot_direction; ///< Currently selected depot direction
static bool _convert_signal_button;          ///< convert signal button in the signal GUI pressed
static SignalVariant _cur_signal_variant;    ///< set the signal variant (for signal GUI)
static SignalType _cur_signal_type;          ///< set the signal type (for signal GUI)
static constexpr int RTHK_POLYRAIL_PREFIX = 1000; ///< Prefix hotkey to combine space with an existing rail toolbar hotkey.
static constexpr int RTHK_POLYRAIL_PREVIEW = 1001; ///< Generate a frozen polyrail preview.
static constexpr int RTHK_POLYRAIL_BUILD_PREVIEW = 1002; ///< Build the frozen polyrail preview.
static constexpr int RTHK_POLYRAIL_REMOVE_PREVIEW = 1003; ///< Remove the newest frozen polyrail preview section.
static constexpr size_t POLYRAIL_MAIN = 0;
static constexpr size_t POLYRAIL_SECONDARY = 1;
static constexpr size_t POLYRAIL_LINE_COUNT = 2;

struct PolyrailStart {
	TileIndex tile = INVALID_TILE; ///< End tile of this rail line.
	Trackdir trackdir = INVALID_TRACKDIR; ///< Direction to continue this rail line.
	TileIndexDiffC offset{0, 0}; ///< Offset from the primary line for cursor projection.
};

static std::optional<std::vector<PolyrailStart>> _polyrail_start; ///< Current polyrail starts, if one has been selected.

struct WaypointPickerSelection {
	StationClassID sel_class; ///< Selected station class.
	uint16_t sel_type; ///< Selected station type within the class.
};
static WaypointPickerSelection _waypoint_gui; ///< Settings of the waypoint picker.

struct StationPickerSelection {
	StationClassID sel_class; ///< Selected station class.
	uint16_t sel_type; ///< Selected station type within the class.
	Axis axis; ///< Selected orientation of the station.
};
static StationPickerSelection _station_gui; ///< Settings of the station picker.


static void HandleStationPlacement(TileIndex start, TileIndex end);
static void ShowBuildTrainDepotPicker(Window *parent);
static void ShowBuildWaypointPicker(Window *parent);
static Window *ShowStationBuilder(Window *parent);
static void ShowSignalBuilder(Window *parent);

/**
 * Check whether a station type can be build.
 * @param statspec The specification of the station, or \c nullptr.
 * @return true if building is allowed.
 */
static bool IsStationAvailable(const StationSpec *statspec)
{
	if (statspec == nullptr || !statspec->callback_mask.Test(StationCallbackMask::Avail)) return true;

	uint16_t cb_res = GetStationCallback(CBID_STATION_AVAILABILITY, 0, 0, statspec, nullptr, INVALID_TILE);
	if (cb_res == CALLBACK_FAILED) return true;

	return Convert8bitBooleanCallback(statspec->grf_prop.grffile, CBID_STATION_AVAILABILITY, cb_res);
}

void CcPlaySound_CONSTRUCTION_RAIL(Commands, const CommandCost &result, TileIndex tile)
{
	if (result.Succeeded() && _settings_client.sound.confirm) SndPlayTileFx(SND_20_CONSTRUCTION_RAIL, tile);
}

static void GenericPlaceRail(TileIndex tile, Track track)
{
	if (_remove_button_clicked) {
		Command<Commands::RemoveRail>::Post(STR_ERROR_CAN_T_REMOVE_RAILROAD_TRACK, CcPlaySound_CONSTRUCTION_RAIL,
				tile, track);
	} else {
		Command<Commands::BuildRail>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_TRACK, CcPlaySound_CONSTRUCTION_RAIL,
				tile, _cur_railtype, track, _settings_client.gui.auto_remove_signals);
	}
}

/**
 * Try to add an additional rail-track at the entrance of a depot
 * @param tile  Tile to use for adding the rail-track
 * @param dir   Direction to check for already present tracks
 * @param track Track to add
 * @see CcRailDepot()
 */
static void PlaceExtraDepotRail(TileIndex tile, DiagDirection dir, Track track)
{
	if (GetRailTileType(tile) == RailTileType::Depot) return;
	if (GetRailTileType(tile) == RailTileType::Signals && !_settings_client.gui.auto_remove_signals) return;
	if ((GetTrackBits(tile) & DiagdirReachesTracks(dir)) == 0) return;

	Command<Commands::BuildRail>::Post(tile, _cur_railtype, track, _settings_client.gui.auto_remove_signals);
}

/** Additional pieces of track to add at the entrance of a depot. */
static const Track _place_depot_extra_track[12] = {
	TRACK_LEFT,  TRACK_UPPER, TRACK_UPPER, TRACK_RIGHT, // First additional track for directions 0..3
	TRACK_X,     TRACK_Y,     TRACK_X,     TRACK_Y,     // Second additional track
	TRACK_LOWER, TRACK_LEFT,  TRACK_RIGHT, TRACK_LOWER, // Third additional track
};

/** Direction to check for existing track pieces. */
static const DiagDirection _place_depot_extra_dir[12] = {
	DIAGDIR_SE, DIAGDIR_SW, DIAGDIR_SE, DIAGDIR_SW,
	DIAGDIR_SW, DIAGDIR_NW, DIAGDIR_NE, DIAGDIR_SE,
	DIAGDIR_NW, DIAGDIR_NE, DIAGDIR_NW, DIAGDIR_NE,
};

void CcRailDepot(Commands, const CommandCost &result, TileIndex tile, RailType, DiagDirection dir)
{
	if (result.Failed()) return;

	if (_settings_client.sound.confirm) SndPlayTileFx(SND_20_CONSTRUCTION_RAIL, tile);
	if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();

	tile += TileOffsByDiagDir(dir);

	if (IsTileType(tile, TileType::Railway)) {
		PlaceExtraDepotRail(tile, _place_depot_extra_dir[dir], _place_depot_extra_track[dir]);

		/* Don't place the rail straight out of the depot of there is another depot across from it. */
		Tile double_depot_tile = tile + TileOffsByDiagDir(dir);
		bool is_double_depot = IsValidTile(double_depot_tile) && IsRailDepotTile(double_depot_tile);
		if (!is_double_depot) PlaceExtraDepotRail(tile, _place_depot_extra_dir[dir + 4], _place_depot_extra_track[dir + 4]);

		PlaceExtraDepotRail(tile, _place_depot_extra_dir[dir + 8], _place_depot_extra_track[dir + 8]);
	}
}

/**
 * Place a rail waypoint.
 * @param tile Position to start dragging a waypoint.
 */
static void PlaceRail_Waypoint(TileIndex tile)
{
	if (_remove_button_clicked) {
		VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_REMOVE_STATION);
		return;
	}

	Axis axis = GetAxisForNewRailWaypoint(tile);
	if (IsValidAxis(axis)) {
		/* Valid tile for waypoints */
		VpStartPlaceSizing(tile, axis == AXIS_X ? VPM_X_LIMITED : VPM_Y_LIMITED, DDSP_BUILD_STATION);
		VpSetPlaceSizingLimit(_settings_game.station.station_spread);
	} else {
		/* Tile where we can't build rail waypoints. This is always going to fail,
		 * but provides the user with a proper error message. */
		Command<Commands::BuildRailWaypoint>::Post(STR_ERROR_CAN_T_BUILD_RAIL_WAYPOINT , tile, AXIS_X, 1, 1, STAT_CLASS_WAYP, 0, StationID::Invalid(), false);
	}
}

void CcStation(Commands, const CommandCost &result, TileIndex tile)
{
	if (result.Failed()) return;

	if (_settings_client.sound.confirm) SndPlayTileFx(SND_20_CONSTRUCTION_RAIL, tile);
	/* Only close the station builder window if the default station and non persistent building is chosen. */
	if (_station_gui.sel_class == STAT_CLASS_DFLT && _station_gui.sel_type == 0 && !_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
}

/**
 * Place a rail station.
 * @param tile Position to place or start dragging a station.
 */
static void PlaceRail_Station(TileIndex tile)
{
	if (_remove_button_clicked) {
		VpStartPlaceSizing(tile, VPM_X_AND_Y_LIMITED, DDSP_REMOVE_STATION);
		VpSetPlaceSizingLimit(-1);
	} else if (_settings_client.gui.station_dragdrop) {
		VpStartPlaceSizing(tile, VPM_X_AND_Y_LIMITED, DDSP_BUILD_STATION);
		VpSetPlaceSizingLimit(_settings_game.station.station_spread);
	} else {
		int w = _settings_client.gui.station_numtracks;
		int h = _settings_client.gui.station_platlength;
		if (!_station_gui.axis) std::swap(w, h);

		StationPickerSelection params = _station_gui;
		RailType rt = _cur_railtype;
		uint8_t numtracks = _settings_client.gui.station_numtracks;
		uint8_t platlength = _settings_client.gui.station_platlength;
		bool adjacent = _ctrl_pressed;

		auto proc = [=](bool test, StationID to_join) -> bool {
			if (test) {
				return Command<Commands::BuildRailStation>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildRailStation>()), tile, rt, params.axis, numtracks, platlength, params.sel_class, params.sel_type, StationID::Invalid(), adjacent).Succeeded();
			} else {
				return Command<Commands::BuildRailStation>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_STATION, CcStation, tile, rt, params.axis, numtracks, platlength, params.sel_class, params.sel_type, to_join, adjacent);
			}
		};

		ShowSelectStationIfNeeded(TileArea(tile, w, h), proc);
	}
}

/**
 * Build a new signal or edit/remove a present signal, use CmdBuildSingleSignal() or CmdRemoveSingleSignal() in rail_cmd.cpp
 *
 * @param tile The tile where the signal will build or edit
 */
static void GenericPlaceSignals(TileIndex tile)
{
	TrackBits trackbits = TrackStatusToTrackBits(GetTileTrackStatus(tile, TRANSPORT_RAIL, RoadTramType::Invalid));

	if (trackbits & TRACK_BIT_VERT) { // N-S direction
		trackbits = (_tile_fract_coords.x <= _tile_fract_coords.y) ? TRACK_BIT_RIGHT : TRACK_BIT_LEFT;
	}

	if (trackbits & TRACK_BIT_HORZ) { // E-W direction
		trackbits = (_tile_fract_coords.x + _tile_fract_coords.y <= 15) ? TRACK_BIT_UPPER : TRACK_BIT_LOWER;
	}

	Track track = FindFirstTrack(trackbits);

	if (_remove_button_clicked) {
		Command<Commands::RemoveSignal>::Post(STR_ERROR_CAN_T_REMOVE_SIGNALS_FROM, CcPlaySound_CONSTRUCTION_RAIL, tile, track);
	} else {
		/* Which signals should we cycle through? */
		bool tile_has_signal = IsPlainRailTile(tile) && IsValidTrack(track) && HasSignalOnTrack(tile, track);
		SignalType cur_signal_on_tile = tile_has_signal ? GetSignalType(tile, track) : _cur_signal_type;
		SignalType cycle_start;
		SignalType cycle_end;

		/* Start with the least restrictive case: the player wants to cycle through all signals they can see. */
		if (_settings_client.gui.cycle_signal_types == SIGNAL_CYCLE_ALL) {
			cycle_start = _settings_client.gui.signal_gui_mode == SIGNAL_GUI_ALL ? SIGTYPE_BLOCK : SIGTYPE_PBS;
			cycle_end = SIGTYPE_LAST;
		} else {
			/* Only cycle through signals of the same group (block or path) as the current signal on the tile. */
			if (cur_signal_on_tile <= SIGTYPE_LAST_NOPBS) {
				/* Block signals only. */
				cycle_start = SIGTYPE_BLOCK;
				cycle_end = SIGTYPE_LAST_NOPBS;
			} else {
				/* Path signals only. */
				cycle_start = SIGTYPE_PBS;
				cycle_end = SIGTYPE_LAST;
			}
		}

		if (FindWindowById(WC_BUILD_SIGNAL, 0) != nullptr) {
			/* signal GUI is used */
			Command<Commands::BuildSignal>::Post(_convert_signal_button ? STR_ERROR_SIGNAL_CAN_T_CONVERT_SIGNALS_HERE : STR_ERROR_CAN_T_BUILD_SIGNALS_HERE, CcPlaySound_CONSTRUCTION_RAIL,
				tile, track, _cur_signal_type, _cur_signal_variant, _convert_signal_button, false, _ctrl_pressed, cycle_start, cycle_end, 0, 0);
		} else {
			SignalVariant sigvar = TimerGameCalendar::year < _settings_client.gui.semaphore_build_before ? SIG_SEMAPHORE : SIG_ELECTRIC;
			Command<Commands::BuildSignal>::Post(STR_ERROR_CAN_T_BUILD_SIGNALS_HERE, CcPlaySound_CONSTRUCTION_RAIL,
				tile, track, _settings_client.gui.default_signal_type, sigvar, false, false, _ctrl_pressed, cycle_start, cycle_end, 0, 0);

		}
	}
}

/**
 * Start placing a rail bridge.
 * @param tile Position of the first tile of the bridge.
 * @param w    Rail toolbar window.
 */
static void PlaceRail_Bridge(TileIndex tile, Window *w)
{
	if (IsBridgeTile(tile)) {
		TileIndex other_tile = GetOtherTunnelBridgeEnd(tile);
		Point pt = {0, 0};
		w->OnPlaceMouseUp(VPM_X_OR_Y, DDSP_BUILD_BRIDGE, pt, other_tile, tile);
	} else {
		VpStartPlaceSizing(tile, VPM_X_OR_Y, DDSP_BUILD_BRIDGE);
	}
}

/**
 * Command callback for building a tunnel.
 * @param result The result of the command.
 * @param tile The tile where the command was executed on.
 */
void CcBuildRailTunnel(Commands, const CommandCost &result, TileIndex tile)
{
	if (result.Succeeded()) {
		if (_settings_client.sound.confirm) SndPlayTileFx(SND_20_CONSTRUCTION_RAIL, tile);
		if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
	} else {
		SetRedErrorSquare(_build_tunnel_endtile);
	}
}

/**
 * Toggles state of the Remove button of Build rail toolbar
 * @param w window the button belongs to
 */
static void ToggleRailButton_Remove(Window *w)
{
	CloseWindowById(WC_SELECT_STATION, 0);
	w->ToggleWidgetLoweredState(WID_RAT_REMOVE);
	w->SetWidgetDirty(WID_RAT_REMOVE);
	_remove_button_clicked = w->IsWidgetLowered(WID_RAT_REMOVE);
	SetSelectionRed(_remove_button_clicked);
}

/**
 * Updates the Remove button because of Ctrl state change
 * @param w window the button belongs to
 * @return true iff the remove button was changed
 */
static bool RailToolbar_CtrlChanged(Window *w)
{
	if (w->IsWidgetDisabled(WID_RAT_REMOVE)) return false;

	/* allow ctrl to switch remove mode only for these widgets */
	for (WidgetID i = WID_RAT_BUILD_NS; i <= WID_RAT_BUILD_STATION; i++) {
		if ((i <= WID_RAT_POLYRAIL_OPTIMIZATION || i >= WID_RAT_BUILD_WAYPOINT) && w->IsWidgetLowered(i)) {
			ToggleRailButton_Remove(w);
			return true;
		}
	}

	return false;
}


/**
 * The "remove"-button click proc of the build-rail toolbar.
 * @param w Build-rail toolbar window
 * @see BuildRailToolbarWindow::OnClick()
 */
static void BuildRailClick_Remove(Window *w)
{
	if (w->IsWidgetDisabled(WID_RAT_REMOVE)) return;
	ToggleRailButton_Remove(w);
	SndClickBeep();

	/* handle station builder */
	if (w->IsWidgetLowered(WID_RAT_BUILD_STATION)) {
		if (_remove_button_clicked) {
			/* starting drag & drop remove */
			if (!_settings_client.gui.station_dragdrop) {
				SetTileSelectSize(1, 1);
			} else {
				VpSetPlaceSizingLimit(-1);
			}
		} else {
			/* starting station build mode */
			if (!_settings_client.gui.station_dragdrop) {
				int x = _settings_client.gui.station_numtracks;
				int y = _settings_client.gui.station_platlength;
				if (_station_gui.axis == 0) std::swap(x, y);
				SetTileSelectSize(x, y);
			} else {
				VpSetPlaceSizingLimit(_settings_game.station.station_spread);
			}
		}
	}
}

static void DoRailroadTrack(Track track)
{
	if (_remove_button_clicked) {
		Command<Commands::RemoveRailLong>::Post(STR_ERROR_CAN_T_REMOVE_RAILROAD_TRACK, CcPlaySound_CONSTRUCTION_RAIL,
				TileVirtXY(_thd.selend.x, _thd.selend.y), TileVirtXY(_thd.selstart.x, _thd.selstart.y), track);
	} else {
		Command<Commands::BuildRailLong>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_TRACK, CcPlaySound_CONSTRUCTION_RAIL,
				TileVirtXY(_thd.selend.x, _thd.selend.y), TileVirtXY(_thd.selstart.x, _thd.selstart.y),  _cur_railtype, track, _settings_client.gui.auto_remove_signals, false);
	}
}

static bool TryOffsetPolyrailTiles(TileIndex start, TileIndex end, int dx, int dy, TileIndex *offset_start, TileIndex *offset_end)
{
	auto try_offset = [&](int offset_x, int offset_y) {
		int sx = static_cast<int>(TileX(start)) + offset_x;
		int sy = static_cast<int>(TileY(start)) + offset_y;
		int ex = static_cast<int>(TileX(end)) + offset_x;
		int ey = static_cast<int>(TileY(end)) + offset_y;
		if (sx < 0 || sy < 0 || ex < 0 || ey < 0) return false;
		if (sx >= static_cast<int>(Map::SizeX()) || ex >= static_cast<int>(Map::SizeX())) return false;
		if (sy >= static_cast<int>(Map::SizeY()) || ey >= static_cast<int>(Map::SizeY())) return false;

		*offset_start = TileXY(sx, sy);
		*offset_end = TileXY(ex, ey);
		return true;
	};

	return try_offset(dx, dy) || try_offset(-dx, -dy);
}

static bool GetParallelPolyrailTrack(TileIndex start, TileIndex end, Track track, TileIndex *parallel_start, TileIndex *parallel_end, Track *parallel_track)
{
	*parallel_start = start;
	*parallel_end = end;

	switch (track) {
		case TRACK_X:
			*parallel_track = TRACK_X;
			return TryOffsetPolyrailTiles(start, end, 0, 1, parallel_start, parallel_end);

		case TRACK_Y:
			*parallel_track = TRACK_Y;
			return TryOffsetPolyrailTiles(start, end, 1, 0, parallel_start, parallel_end);

		case TRACK_UPPER:
			*parallel_track = TRACK_UPPER;
			return TryOffsetPolyrailTiles(start, end, 0, 1, parallel_start, parallel_end);

		case TRACK_LOWER:
			*parallel_track = TRACK_LOWER;
			return TryOffsetPolyrailTiles(start, end, 0, -1, parallel_start, parallel_end);

		case TRACK_LEFT:
			*parallel_track = TRACK_LEFT;
			return TryOffsetPolyrailTiles(start, end, 1, 0, parallel_start, parallel_end);

		case TRACK_RIGHT:
			*parallel_track = TRACK_RIGHT;
			return TryOffsetPolyrailTiles(start, end, -1, 0, parallel_start, parallel_end);

		default:
			return false;
	}
}

static void PostPolyrailTrackCommand(TileIndex start, TileIndex end, Track track)
{
	if (_remove_button_clicked) {
		Command<Commands::RemoveRailLong>::Post(STR_ERROR_CAN_T_REMOVE_RAILROAD_TRACK, CcPlaySound_CONSTRUCTION_RAIL, end, start, track);
	} else {
		Command<Commands::BuildRailLong>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_TRACK, CcPlaySound_CONSTRUCTION_RAIL,
				end, start, _cur_railtype, track, _settings_client.gui.auto_remove_signals, false);
	}
}

static void DoPolyrailTrack(Track track)
{
	TileIndex start = TileVirtXY(_thd.selstart.x, _thd.selstart.y);
	TileIndex end = TileVirtXY(_thd.selend.x, _thd.selend.y);

	PostPolyrailTrackCommand(start, end, track);

	TileIndex parallel_start;
	TileIndex parallel_end;
	Track parallel_track;
	if (GetParallelPolyrailTrack(start, end, track, &parallel_start, &parallel_end, &parallel_track)) {
		PostPolyrailTrackCommand(parallel_start, parallel_end, parallel_track);
	}
}

static void HandleAutodirPlacement()
{
	Track trackstat = static_cast<Track>( _thd.drawstyle & HT_DIR_MASK); // 0..5

	if (_thd.drawstyle & HT_RAIL) { // one tile case
		GenericPlaceRail(TileVirtXY(_thd.selend.x, _thd.selend.y), trackstat);
		return;
	}

	DoRailroadTrack(trackstat);
}

static void HandlePolyrailPlacement()
{
	Track trackstat = static_cast<Track>(_thd.drawstyle & HT_DIR_MASK); // 0..5
	DoPolyrailTrack(trackstat);
}

struct PolyrailSegment {
	TileIndex start = INVALID_TILE;
	TileIndex end = INVALID_TILE;
	Track track = INVALID_TRACK;
	Trackdir start_trackdir = INVALID_TRACKDIR;
	Trackdir end_trackdir = INVALID_TRACKDIR;
	TileIndex height_ramp_start = INVALID_TILE;
	Trackdir height_ramp_trackdir = INVALID_TRACKDIR;
	TileIndex height_reference_tile = INVALID_TILE;
	int height_reference = 0;
	TileIndex height_anchor_tile = INVALID_TILE;
	TileIndex height_prefix_end_tile = INVALID_TILE;
	Trackdir height_last_on_grid_trackdir = INVALID_TRACKDIR;
};

struct PolyrailOptimizationRouteSettings {
	std::vector<PolyrailStart> starts;
	std::vector<PolyrailStart> ends;
};

struct PolyrailOptimizationSegment {
	Direction dir = INVALID_DIR;
	uint length = 0;
};

struct PolyrailOptimizationEnd {
	TileIndex tile = INVALID_TILE;
	Trackdir trackdir = INVALID_TRACKDIR;
};

struct PolyrailOptimizationBridge {
	TileIndex start = INVALID_TILE;
	TileIndex end = INVALID_TILE;
	BridgeType bridge_type = 0;
	Track track = INVALID_TRACK;
};

struct PolyrailTerraformCommand {
	TileIndex tile = INVALID_TILE;
	Slope slope = SLOPE_FLAT;
	bool dir_up = false;
};

struct PolyrailEqualizationCommand {
	TileIndex start_tile = INVALID_TILE;
	TileIndex end_tile = INVALID_TILE;
	bool post_level = true;
};

struct PolyrailLevelLandArea {
	TileIndex start_index = INVALID_TILE;
	TileIndex end_index = INVALID_TILE;
};

struct PolyrailOptimizationBuildPlan {
	std::vector<PolyrailTerraformCommand> terraforms;
	std::vector<PolyrailEqualizationCommand> equalizations;
	std::vector<PolyrailSegment> rails;
	std::vector<PolyrailOptimizationBridge> bridges;
	std::vector<PolyrailSegment> final_route;
};

struct PolyrailPreviewState {
	std::vector<std::vector<PolyrailSegment>> stored_routes; ///< Accepted preview sections, drawn grey.
	std::optional<std::vector<PolyrailSegment>> active_route; ///< Current preview section, drawn white.
	std::optional<std::vector<PolyrailStart>> build_starts; ///< Virtual starts after all stored preview sections.
};

static PolyrailPreviewState _polyrail_preview; ///< Frozen polyrail route previews, if any have been generated.

static void AddPolyrailPreviewSegments(const std::vector<PolyrailSegment> &route, bool grey, std::vector<PolyrailHighlightSegment> *preview)
{
	preview->reserve(preview->size() + route.size());
	for (const PolyrailSegment &segment : route) {
		preview->push_back({segment.start, segment.end, segment.track, grey});
	}
}

static std::string FormatPolyrailTile(TileIndex tile)
{
	if (tile == INVALID_TILE) return "INVALID_TILE";
	if (tile >= Map::Size()) return fmt::format("OUT_OF_MAP({})", tile.base());
	return fmt::format("({}, {})", TileX(tile), TileY(tile));
}

static void LogPolyrailPreviewRoute(const std::vector<PolyrailSegment> &route)
{
	Debug(misc, 1, "Polyrail preview route: segments={}", route.size());
	if (route.size() < POLYRAIL_LINE_COUNT || route.size() % POLYRAIL_LINE_COUNT != 0) {
		Debug(misc, 1, "Polyrail preview route: cannot split into two lines");
		for (size_t i = 0; i < route.size(); i++) {
			Debug(misc, 1, "Polyrail preview route: segment={} start={} end={}",
					i, FormatPolyrailTile(route[i].start), FormatPolyrailTile(route[i].end));
		}
		return;
	}

	size_t segment_count = route.size() / POLYRAIL_LINE_COUNT;
	for (size_t line = 0; line < POLYRAIL_LINE_COUNT; line++) {
		for (size_t segment = 0; segment < segment_count; segment++) {
			size_t route_index = segment + 1 == segment_count ?
					(POLYRAIL_LINE_COUNT * (segment_count - 1)) + line :
					(line * (segment_count - 1)) + segment;
			Debug(misc, 1, "Polyrail preview route: line={} segment={} start={} end={}",
					line, segment, FormatPolyrailTile(route[route_index].start), FormatPolyrailTile(route[route_index].end));
		}
	}
}

static void UpdatePolyrailPreviewHighlight()
{
	std::vector<PolyrailHighlightSegment> preview;
	for (const std::vector<PolyrailSegment> &route : _polyrail_preview.stored_routes) {
		AddPolyrailPreviewSegments(route, true, &preview);
	}
	if (_polyrail_preview.active_route.has_value()) {
		AddPolyrailPreviewSegments(*_polyrail_preview.active_route, false, &preview);
	}

	SetPolyrailPreviewSegments(preview);
}

static void ClearPolyrailPreview()
{
	_polyrail_preview = {};
	ClearPolyrailPreviewSegments();
}

static void SetPolyrailPreview(const std::vector<PolyrailSegment> &route)
{
	_polyrail_preview.stored_routes.clear();
	_polyrail_preview.build_starts.reset();
	_polyrail_preview.active_route = route;
	LogPolyrailPreviewRoute(route);
	UpdatePolyrailPreviewHighlight();
}

/* PolyRail geometry helpers. */

static TileIndexDiffC GetPolyrailTrackdirDelta(Trackdir trackdir)
{
	static const TileIndexDiffC track_delta[] = {
		{ -1,  0 }, {  0,  1 }, { -1,  0 }, {  0,  1 }, {  1,  0 }, {  0,  1 },
		{  0,  0 }, {  0,  0 },
		{  1,  0 }, {  0, -1 }, {  0, -1 }, {  1,  0 }, {  0, -1 }, { -1,  0 },
		{  0,  0 }, {  0,  0 },
	};
	static_assert(std::size(track_delta) == TRACKDIR_END);
	return track_delta[trackdir];
}

static Trackdir AdvancePolyrailTrackdir(Trackdir trackdir)
{
	if (!IsDiagonalTrackdir(trackdir)) ToggleBit(trackdir, 0);
	return trackdir;
}

static TileIndex StepPolyrailTrackdir(TileIndex tile, Trackdir *trackdir)
{
	TileIndex next = AddTileIndexDiffCWrap(tile, GetPolyrailTrackdirDelta(*trackdir));
	if (next == INVALID_TILE) return INVALID_TILE;

	*trackdir = AdvancePolyrailTrackdir(*trackdir);
	return next;
}

static Direction GetPolyrailTrackdirDirection(Trackdir trackdir)
{
	switch (trackdir) {
		case TRACKDIR_X_NE: return DIR_NE;
		case TRACKDIR_Y_SE: return DIR_SE;
		case TRACKDIR_UPPER_E:
		case TRACKDIR_LOWER_E: return DIR_E;
		case TRACKDIR_LEFT_S:
		case TRACKDIR_RIGHT_S: return DIR_S;
		case TRACKDIR_X_SW: return DIR_SW;
		case TRACKDIR_Y_NW: return DIR_NW;
		case TRACKDIR_UPPER_W:
		case TRACKDIR_LOWER_W: return DIR_W;
		case TRACKDIR_LEFT_N:
		case TRACKDIR_RIGHT_N: return DIR_N;
		default: NOT_REACHED();
	}
}

static bool IsPolyrailTransitionAllowed(Trackdir previous, Trackdir next)
{
	if (previous == INVALID_TRACKDIR) return true;
	if (next == INVALID_TRACKDIR) return false;

	Direction previous_dir = GetPolyrailTrackdirDirection(previous);
	Direction next_dir = GetPolyrailTrackdirDirection(next);
	return next_dir == previous_dir ||
			next_dir == ChangeDir(previous_dir, DIRDIFF_45RIGHT) ||
			next_dir == ChangeDir(previous_dir, DIRDIFF_45LEFT);
}

static Trackdir GetPolyrailConnectionTrackdir(Trackdir previous, Direction dir)
{
	if (previous == INVALID_TRACKDIR) return INVALID_TRACKDIR;

	Direction previous_dir = GetPolyrailTrackdirDirection(previous);
	if (dir == previous_dir) return IsDiagonalTrackdir(previous) ? previous : NextTrackdir(previous);

	if (IsDiagonalTrackdir(previous)) {
		switch (previous) {
			case TRACKDIR_X_NE:
				if (dir == DIR_N) return TRACKDIR_LEFT_N;
				if (dir == DIR_E) return TRACKDIR_LOWER_E;
				break;

			case TRACKDIR_Y_SE:
				if (dir == DIR_E) return TRACKDIR_UPPER_E;
				if (dir == DIR_S) return TRACKDIR_LEFT_S;
				break;

			case TRACKDIR_X_SW:
				if (dir == DIR_S) return TRACKDIR_RIGHT_S;
				if (dir == DIR_W) return TRACKDIR_UPPER_W;
				break;

			case TRACKDIR_Y_NW:
				if (dir == DIR_W) return TRACKDIR_LOWER_W;
				if (dir == DIR_N) return TRACKDIR_RIGHT_N;
				break;

			default:
				break;
		}
	} else if (IsDiagonalDirection(dir)) {
		return TrackDirectionToTrackdir(DiagDirToDiagTrack(DirToDiagDir(dir)), dir);
	}

	return INVALID_TRACKDIR;
}

static bool CanPolyrailTurnStraightToDiagonal(Trackdir previous, Direction dir)
{
	return !IsDiagonalTrackdir(previous) && IsDiagonalDirection(dir) && TrackdirToExitdir(previous) == DirToDiagDir(dir);
}

static Trackdir GetPolyrailTargetTrackdir(Trackdir previous, Direction dir)
{
	Trackdir target = GetPolyrailConnectionTrackdir(previous, dir);
	if (target != INVALID_TRACKDIR) return target;

	if (previous != INVALID_TRACKDIR && GetPolyrailTrackdirDirection(previous) == dir) {
		return IsDiagonalTrackdir(previous) ? previous : NextTrackdir(previous);
	}

	return INVALID_TRACKDIR;
}

static Trackdir GetCanonicalPolyrailEndTrackdir(Trackdir trackdir)
{
	switch (GetPolyrailTrackdirDirection(trackdir)) {
		case DIR_N: return TRACKDIR_LEFT_N;
		case DIR_S: return TRACKDIR_RIGHT_S;
		case DIR_W: return TRACKDIR_UPPER_W;
		case DIR_E: return TRACKDIR_LOWER_E;
		default: return trackdir;
	}
}

static TileIndex ProjectPolyrailEnd(TileIndex start, TileIndex cursor, Trackdir trackdir, Trackdir *end_trackdir = nullptr, uint *end_steps = nullptr)
{
	TileIndex best = start;
	uint best_distance = DistanceManhattan(start, cursor);
	Trackdir best_trackdir = trackdir;
	uint best_steps = 0;
	Trackdir canonical_end = GetCanonicalPolyrailEndTrackdir(trackdir);

	auto consider = [&](TileIndex tile, Trackdir tile_trackdir, uint steps) {
		uint distance = DistanceManhattan(tile, cursor);
		if (tile_trackdir == canonical_end && (best_trackdir != canonical_end || distance <= best_distance)) {
			best = tile;
			best_distance = distance;
			best_trackdir = tile_trackdir;
			best_steps = steps;
			return;
		}
		if (best_trackdir != canonical_end && distance <= best_distance) {
			best = tile;
			best_distance = distance;
			best_trackdir = tile_trackdir;
			best_steps = steps;
		}
	};

	TileIndex tile = start;
	uint max_steps = DistanceManhattan(start, cursor) + 4;
	for (uint step = 0; step < max_steps; step++) {
		TileIndex next = StepPolyrailTrackdir(tile, &trackdir);
		if (next == INVALID_TILE) break;
		tile = next;

		consider(tile, trackdir, step + 1);
		if (DistanceManhattan(start, tile) > DistanceManhattan(start, cursor) + 3) {
			break;
		}
	}

	if (end_trackdir != nullptr) *end_trackdir = best_trackdir;
	if (end_steps != nullptr) *end_steps = best_steps;
	return best;
}

static TileIndex ProjectPolyrailEndBySteps(TileIndex start, Trackdir trackdir, uint steps, Trackdir *end_trackdir)
{
	TileIndex tile = start;
	for (uint step = 0; step < steps; step++) {
		TileIndex next = StepPolyrailTrackdir(tile, &trackdir);
		if (next == INVALID_TILE) return INVALID_TILE;
		tile = next;
	}

	if (end_trackdir != nullptr) *end_trackdir = trackdir;
	return tile;
}

static TileIndexDiffC GetPolyrailTileOffset(TileIndex from, TileIndex to)
{
	return {
		static_cast<int16_t>(static_cast<int>(TileX(to)) - static_cast<int>(TileX(from))),
		static_cast<int16_t>(static_cast<int>(TileY(to)) - static_cast<int>(TileY(from))),
	};
}

static bool TryGetDefaultPolyrailOffset(Track track, TileIndex start, TileIndex end, TileIndexDiffC *offset);

static Direction ChoosePolyrailDirection(TileIndex start, TileIndex cursor, Trackdir preferred_trackdir)
{
	int dx = static_cast<int>(TileX(cursor)) - static_cast<int>(TileX(start));
	int dy = static_cast<int>(TileY(cursor)) - static_cast<int>(TileY(start));

	std::vector<Direction> directions;
	if (preferred_trackdir != INVALID_TRACKDIR) {
		Direction forward = GetPolyrailTrackdirDirection(preferred_trackdir);
		directions = {
			forward,
			ChangeDir(forward, DIRDIFF_45RIGHT),
			ChangeDir(forward, DIRDIFF_45LEFT),
		};
	} else {
		for (Direction dir = DIR_BEGIN; dir != DIR_END; dir++) directions.push_back(dir);
	}

	Direction best = INVALID_DIR;
	int64_t best_score = INT64_MIN;
	int best_len_sq = 1;
	int best_dot = INT_MIN;

	for (Direction dir : directions) {
		TileIndexDiffC diff = TileIndexDiffCByDir(dir);
		int dot = dx * diff.x + dy * diff.y;
		if (dot <= 0) continue;
		int len_sq = diff.x * diff.x + diff.y * diff.y;
		int64_t dir_score = static_cast<int64_t>(dot) * dot;

		int64_t lhs = dir_score * best_len_sq;
		int64_t rhs = best_score * len_sq;
		if (best == INVALID_DIR || lhs > rhs || (lhs == rhs && dot > best_dot)) {
			best = dir;
			best_score = dir_score;
			best_len_sq = len_sq;
			best_dot = dot;
		}
	}

	return best;
}

static Direction ChoosePolyrailDirectionFromHighlight(TileIndex start, TileIndex end, HighLightStyle drawstyle)
{
	if (!(drawstyle & (HT_RAIL | HT_LINE))) return INVALID_DIR;

	Track track = static_cast<Track>(drawstyle & HT_DIR_MASK);
	if (track >= TRACK_END) return INVALID_DIR;

	int dx = static_cast<int>(TileX(end)) - static_cast<int>(TileX(start));
	int dy = static_cast<int>(TileY(end)) - static_cast<int>(TileY(start));

	Direction best = INVALID_DIR;
	int best_dot = INT_MIN;
	for (Direction dir = DIR_BEGIN; dir != DIR_END; dir++) {
		if (TrackDirectionToTrackdir(track, dir) == INVALID_TRACKDIR) continue;

		TileIndexDiffC diff = TileIndexDiffCByDir(dir);
		int dot = dx * diff.x + dy * diff.y;
		if (dot > best_dot) {
			best = dir;
			best_dot = dot;
		}
	}

	return best_dot > 0 ? best : INVALID_DIR;
}

static Trackdir GetPolyrailTrackdirForDirection(Direction dir)
{
	for (Track track = TRACK_BEGIN; track != TRACK_END; track++) {
		Trackdir trackdir = TrackDirectionToTrackdir(track, dir);
		if (trackdir != INVALID_TRACKDIR) return trackdir;
	}
	return INVALID_TRACKDIR;
}

static uint GetDirectionTurnDistance(Direction start_dir, Direction end_dir)
{
	uint turn_right = (static_cast<uint>(end_dir) + DIR_END - static_cast<uint>(start_dir)) % DIR_END;
	return std::min(turn_right, DIR_END - turn_right);
}

static bool DoesDirectionConnect(TileIndex start, TileIndex end, Direction dir)
{
	int dx = static_cast<int>(TileX(end)) - static_cast<int>(TileX(start));
	int dy = static_cast<int>(TileY(end)) - static_cast<int>(TileY(start));
	TileIndexDiffC delta = TileIndexDiffCByDir(dir);

	if (dx == 0 && dy == 0) return true;
	if (delta.x == 0) return dx == 0 && dy * delta.y > 0;
	if (delta.y == 0) return dy == 0 && dx * delta.x > 0;
	return dx * delta.y == dy * delta.x && dx * delta.x > 0 && dy * delta.y > 0;
}

/* PolyRail Optimization route-shape helpers. */

static int ScorePolyrailDirection(TileIndex start, TileIndex end, Direction dir)
{
	int dx = static_cast<int>(TileX(end)) - static_cast<int>(TileX(start));
	int dy = static_cast<int>(TileY(end)) - static_cast<int>(TileY(start));
	TileIndexDiffC delta = TileIndexDiffCByDir(dir);
	return dx * delta.x + dy * delta.y;
}

static int ScorePolyrailDirectionTurn(TileIndex start, TileIndex end, Direction start_dir, uint turn_count, int turn_step)
{
	int score = 0;
	Direction dir = start_dir;
	for (uint i = 1; i < turn_count; i++) {
		dir = ChangeDir(dir, turn_step > 0 ? DIRDIFF_45RIGHT : DIRDIFF_45LEFT);
		score += ScorePolyrailDirection(start, end, dir);
	}
	return score;
}

static int ChoosePolyrailDirectionTurnStep(TileIndex start, TileIndex end, Direction start_dir, Direction end_dir)
{
	uint turn_right = (static_cast<uint>(end_dir) + DIR_END - static_cast<uint>(start_dir)) % DIR_END;
	if (turn_right == 0) return 0;
	if (turn_right < DIRDIFF_REVERSE) return 1;
	if (turn_right > DIRDIFF_REVERSE) return -1;

	uint turn_count = GetDirectionTurnDistance(start_dir, end_dir);
	int right_score = ScorePolyrailDirectionTurn(start, end, start_dir, turn_count, 1);
	int left_score = ScorePolyrailDirectionTurn(start, end, start_dir, turn_count, -1);
	return right_score >= left_score ? 1 : -1;
}

static std::optional<std::vector<Direction>> BuildDirectionSequence(Direction start_dir, Direction end_dir, TileIndex start, TileIndex end)
{
	if (start_dir == INVALID_DIR || end_dir == INVALID_DIR || start == INVALID_TILE || end == INVALID_TILE) return std::nullopt;

	std::vector<Direction> directions;
	if (start_dir == end_dir) {
		if (DoesDirectionConnect(start, end, start_dir)) {
			directions.push_back(start_dir);
			return directions;
		}

		Direction right = ChangeDir(start_dir, DIRDIFF_45RIGHT);
		Direction left = ChangeDir(start_dir, DIRDIFF_45LEFT);
		Direction deviation = ScorePolyrailDirection(start, end, right) >= ScorePolyrailDirection(start, end, left) ? right : left;
		directions = {start_dir, deviation, end_dir};
		return directions;
	}

	uint turn_count = GetDirectionTurnDistance(start_dir, end_dir);
	int turn_step = ChoosePolyrailDirectionTurnStep(start, end, start_dir, end_dir);
	if (turn_step == 0) return std::nullopt;

	directions.reserve(turn_count + 1);
	Direction dir = start_dir;
	directions.push_back(dir);
	for (uint i = 0; i < turn_count; i++) {
		dir = ChangeDir(dir, turn_step > 0 ? DIRDIFF_45RIGHT : DIRDIFF_45LEFT);
		directions.push_back(dir);
	}
	return directions;
}

static uint GetPolyrailOptimizationInitialLength(TileIndex start, TileIndex end)
{
	return std::max<uint>(DistanceManhattan(start, end), 7);
}

static bool IsPolyrailOptimizationAnyLengthDirection(Direction dir)
{
	Trackdir trackdir = GetPolyrailTrackdirForDirection(dir);
	return trackdir != INVALID_TRACKDIR && IsDiagonalTrackdir(trackdir);
}

static bool NeedsEvenPolyrailOptimizationLength(Direction previous_dir, Direction next_dir)
{
	return previous_dir != next_dir;
}

static uint AdjustPolyrailOptimizationLengthForSegment(uint length, Direction dir, Direction previous_dir, Direction next_dir)
{
	if (IsPolyrailOptimizationAnyLengthDirection(dir)) return length;

	bool needs_even = NeedsEvenPolyrailOptimizationLength(previous_dir, next_dir);
	if ((length % 2 == 0) != needs_even) length++;
	return length;
}

static std::optional<std::vector<PolyrailOptimizationSegment>> BuildPolyrailOptimizationSegments(const std::vector<Direction> &directions, TileIndex start, TileIndex end)
{
	if (directions.empty()) return std::nullopt;

	uint initial_length = GetPolyrailOptimizationInitialLength(start, end);
	std::vector<PolyrailOptimizationSegment> segments;
	segments.reserve(directions.size());
	for (size_t i = 0; i < directions.size(); i++) {
		Direction previous_dir = i == 0 ? directions[i] : directions[i - 1];
		Direction next_dir = i + 1 == directions.size() ? directions[i] : directions[i + 1];
		uint length = AdjustPolyrailOptimizationLengthForSegment(initial_length, directions[i], previous_dir, next_dir);
		segments.push_back({directions[i], length});
	}
	return segments;
}

static TileIndex GetPolyrailOptimizationContinuationTile(TileIndex tile, Trackdir trackdir)
{
	if (tile == INVALID_TILE || trackdir == INVALID_TRACKDIR) return INVALID_TILE;
	return AddTileIndexDiffCWrap(tile, GetPolyrailTrackdirDelta(trackdir));
}

static Trackdir GetPolyrailOptimizationSegmentTrackdir(Trackdir previous, Direction dir)
{
	Trackdir trackdir = GetPolyrailTargetTrackdir(previous, dir);
	return trackdir != INVALID_TRACKDIR ? trackdir : GetPolyrailTrackdirForDirection(dir);
}

static std::optional<PolyrailOptimizationEnd> EvaluatePolyrailOptimizationSegments(const PolyrailStart &start, const std::vector<PolyrailOptimizationSegment> &segments)
{
	if (start.tile == INVALID_TILE || segments.empty()) return std::nullopt;

	TileIndex tile = start.tile;
	Trackdir end_trackdir = start.trackdir;
	for (size_t i = 0; i < segments.size(); i++) {
		const PolyrailOptimizationSegment &segment = segments[i];
		Trackdir trackdir = GetPolyrailOptimizationSegmentTrackdir(end_trackdir, segment.dir);
		if (trackdir == INVALID_TRACKDIR) return std::nullopt;

		tile = ProjectPolyrailEndBySteps(tile, trackdir, segment.length, &end_trackdir);
		if (tile == INVALID_TILE || end_trackdir == INVALID_TRACKDIR) return std::nullopt;

		if (i + 1 != segments.size()) {
			tile = GetPolyrailOptimizationContinuationTile(tile, end_trackdir);
			if (tile == INVALID_TILE) return std::nullopt;
		}
	}

	return PolyrailOptimizationEnd{tile, end_trackdir};
}

static uint ScorePolyrailOptimizationEnd(const PolyrailOptimizationEnd &result, const PolyrailStart &desired_end)
{
	if (result.tile == INVALID_TILE || result.trackdir == INVALID_TRACKDIR || desired_end.tile == INVALID_TILE || desired_end.trackdir == INVALID_TRACKDIR) return UINT_MAX;

	uint score = DistanceManhattan(result.tile, desired_end.tile);
	if (GetPolyrailTrackdirDirection(result.trackdir) != GetPolyrailTrackdirDirection(desired_end.trackdir)) score += 10000;
	return score;
}

static uint GetPolyrailOptimizationLengthStep(Direction dir)
{
	return IsPolyrailOptimizationAnyLengthDirection(dir) ? 1 : 2;
}

static bool TryScorePolyrailOptimizationCandidate(const PolyrailStart &start, const PolyrailStart &desired_end, std::vector<PolyrailOptimizationSegment> &segments, uint *score)
{
	std::optional<PolyrailOptimizationEnd> result = EvaluatePolyrailOptimizationSegments(start, segments);
	if (!result.has_value()) return false;

	*score = ScorePolyrailOptimizationEnd(*result, desired_end);
	return true;
}

static bool OptimizePolyrailOptimizationSegmentLengths(const PolyrailStart &start, const PolyrailStart &desired_end, std::vector<PolyrailOptimizationSegment> &segments)
{
	uint best_score;
	if (!TryScorePolyrailOptimizationCandidate(start, desired_end, segments, &best_score)) return false;

	for (uint iteration = 0; iteration < 1000 && best_score != 0; iteration++) {
		bool improved = false;

		for (PolyrailOptimizationSegment &segment : segments) {
			uint original_length = segment.length;
			uint step = GetPolyrailOptimizationLengthStep(segment.dir);
			uint best_length = original_length;

			segment.length = original_length + step;
			uint candidate_score;
			if (TryScorePolyrailOptimizationCandidate(start, desired_end, segments, &candidate_score) && candidate_score < best_score) {
				best_score = candidate_score;
				best_length = segment.length;
			}

			if (original_length > step) {
				segment.length = original_length - step;
				if (TryScorePolyrailOptimizationCandidate(start, desired_end, segments, &candidate_score) && candidate_score < best_score) {
					best_score = candidate_score;
					best_length = segment.length;
				}
			}

			segment.length = best_length;
			if (best_length != original_length) {
				improved = true;
				if (best_score == 0) break;
			}
		}

		if (!improved) break;
	}

	return true;
}

static bool HasPolyrailTrack(TileIndex tile, Trackdir trackdir)
{
	if (tile == INVALID_TILE || trackdir == INVALID_TRACKDIR || !IsPlainRailTile(tile)) return false;
	return (GetTrackBits(tile) & TrackToTrackBits(TrackdirToTrack(trackdir))) != 0;
}

static bool AppendPolyrailLandRun(TileIndex start, Trackdir trackdir, uint steps, std::vector<PolyrailSegment> *rails, bool include_single_tile);

static bool IsPolyrailOnGridTrack(Track track)
{
	return track == TRACK_X || track == TRACK_Y;
}

static bool IsPolyrailOffGridTrack(Track track)
{
	return track == TRACK_UPPER || track == TRACK_LOWER || track == TRACK_LEFT || track == TRACK_RIGHT;
}

static bool DoesPolyrailTileTerrainChangeHeight(TileIndex tile)
{
	return tile != INVALID_TILE && RemoveHalftileSlope(GetTileSlope(tile)) != SLOPE_FLAT;
}

enum class PolyrailRailBuildValidity {
	Valid,
	WrongSlope,
	Invalid,
};

struct PolyrailHeightTarget {
	TileIndex reference_tile = INVALID_TILE;
	int reference_height = 0;
};

struct PolyrailHeightDetection {
	bool found = false;
	size_t segment_index = 0;
	Trackdir segment_trackdir = INVALID_TRACKDIR;
	uint prefix_tiles = 0;
	Trackdir last_on_grid_trackdir = INVALID_TRACKDIR;
};

struct PolyrailLineHeightAnalysis {
	std::vector<PolyrailSegment> route;
	PolyrailHeightDetection detection;
};

static PolyrailRailBuildValidity TestPolyrailRailBuildValidity(TileIndex tile, Track track)
{
	CommandCost ret = Command<Commands::BuildRail>::Do(
			CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildRail>()) | DoCommandFlag::QueryCost,
			tile, _cur_railtype, track, _settings_client.gui.auto_remove_signals);
	if (ret.Succeeded() || ret.GetErrorMessage() == STR_ERROR_ALREADY_BUILT) return PolyrailRailBuildValidity::Valid;
	if (ret.GetErrorMessage() == STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION) return PolyrailRailBuildValidity::WrongSlope;
	return PolyrailRailBuildValidity::Invalid;
}

static std::optional<PolyrailHeightTarget> FindPolyrailHeightRampTarget(TileIndex start, Trackdir trackdir)
{
	if (start == INVALID_TILE || trackdir == INVALID_TRACKDIR) return std::nullopt;

	TileIndex tile = start;
	for (uint i = 0; i < 10; i++) {
		tile = StepPolyrailTrackdir(tile, &trackdir);
		if (tile == INVALID_TILE) return std::nullopt;

		if (IsTileFlat(tile)) return PolyrailHeightTarget{tile, static_cast<int>(TileHeight(tile))};
	}

	return std::nullopt;
}

static std::optional<PolyrailLineHeightAnalysis> AnalyzePolyrailLineRouteForHeight(std::vector<PolyrailSegment> route, Trackdir previous_on_grid_trackdir)
{
	PolyrailLineHeightAnalysis analysis;
	analysis.route = std::move(route);
	if (_remove_button_clicked) return analysis;

		for (size_t segment_index = 0; segment_index < analysis.route.size(); segment_index++) {
			const PolyrailSegment &segment = analysis.route[segment_index];
			Trackdir segment_trackdir = segment.start_trackdir;
			if (segment_trackdir == INVALID_TRACKDIR) return std::nullopt;

		TileIndex tile = segment.start;
		uint run_steps = 0;
		Trackdir trackdir = segment_trackdir;

		for (uint guard = 0; guard < Map::Size(); guard++) {
			Track track = TrackdirToTrack(trackdir);
			PolyrailRailBuildValidity validity = TestPolyrailRailBuildValidity(tile, track);
			if (validity == PolyrailRailBuildValidity::Invalid) return std::nullopt;

			bool needs_height_change = IsPolyrailOffGridTrack(track) && DoesPolyrailTileTerrainChangeHeight(tile);
			if (needs_height_change) {
				if (previous_on_grid_trackdir == INVALID_TRACKDIR) return analysis;

				uint prefix_tiles = run_steps;
				if (prefix_tiles % 2 != 0) prefix_tiles--;
				if (prefix_tiles == 0) return analysis;

				analysis.detection = {true, segment_index, segment_trackdir, prefix_tiles, previous_on_grid_trackdir};
				return analysis;
			}
			if (validity == PolyrailRailBuildValidity::WrongSlope) return std::nullopt;

			if (IsPolyrailOnGridTrack(track)) previous_on_grid_trackdir = trackdir;
			if (tile == segment.end) break;

			TileIndex next = StepPolyrailTrackdir(tile, &trackdir);
			if (next == INVALID_TILE) return std::nullopt;
			tile = next;
			run_steps++;
		}

		if (tile != segment.end) return std::nullopt;
	}

	return analysis;
}

static std::optional<PolyrailHeightDetection> GetPolyrailFallbackHeightDetection(const std::vector<PolyrailSegment> &route, Trackdir previous_on_grid_trackdir)
{
	for (size_t segment_index = 0; segment_index < route.size(); segment_index++) {
		const PolyrailSegment &segment = route[segment_index];
		Trackdir segment_trackdir = segment.start_trackdir;
		if (segment_trackdir == INVALID_TRACKDIR) return std::nullopt;

		if (IsPolyrailOffGridTrack(segment.track)) {
			if (previous_on_grid_trackdir == INVALID_TRACKDIR) return std::nullopt;
			return PolyrailHeightDetection{true, segment_index, segment_trackdir, 0, previous_on_grid_trackdir};
		}

		Trackdir trackdir = segment_trackdir;
		TileIndex tile = segment.start;
		for (uint guard = 0; guard < Map::Size(); guard++) {
			if (IsPolyrailOnGridTrack(TrackdirToTrack(trackdir))) previous_on_grid_trackdir = trackdir;
			if (tile == segment.end) break;

			tile = StepPolyrailTrackdir(tile, &trackdir);
			if (tile == INVALID_TILE) return std::nullopt;
		}
	}

	return std::nullopt;
}

static std::optional<std::vector<PolyrailSegment>> ApplyPolyrailHeightPrefix(const PolyrailLineHeightAnalysis &analysis, const PolyrailHeightDetection &detection, uint prefix_tiles)
{
	if (prefix_tiles == 0 || detection.segment_index >= analysis.route.size()) return analysis.route;

	std::vector<PolyrailSegment> adapted;
	adapted.reserve(detection.segment_index + 2);
	for (size_t i = 0; i < detection.segment_index; i++) {
		adapted.push_back(analysis.route[i]);
	}

	const PolyrailSegment &segment = analysis.route[detection.segment_index];
	if (!AppendPolyrailLandRun(segment.start, detection.segment_trackdir, prefix_tiles - 1, &adapted, false)) return std::nullopt;
	if (adapted.empty()) return std::nullopt;

	Trackdir anchor_trackdir = INVALID_TRACKDIR;
	TileIndex anchor = ProjectPolyrailEndBySteps(segment.start, detection.segment_trackdir, prefix_tiles, &anchor_trackdir);
	if (anchor == INVALID_TILE) return std::nullopt;

	std::optional<PolyrailHeightTarget> target = FindPolyrailHeightRampTarget(anchor, detection.last_on_grid_trackdir);
	size_t prefix_index = adapted.size() - 1;
	adapted[prefix_index].height_anchor_tile = anchor;
	adapted[prefix_index].height_prefix_end_tile = adapted[prefix_index].end;
	adapted[prefix_index].height_last_on_grid_trackdir = detection.last_on_grid_trackdir;
	if (target.has_value()) {
		adapted[prefix_index].height_reference_tile = target->reference_tile;
		adapted[prefix_index].height_reference = target->reference_height;

		uint height_difference = static_cast<uint>(std::abs(target->reference_height - GetTileZ(anchor)));
		Debug(misc, 1, "Polyrail on-grid ramp: height_diff={}", height_difference);
		if (height_difference > 0) {
			adapted[prefix_index].height_ramp_start = anchor;
			adapted[prefix_index].height_ramp_trackdir = detection.last_on_grid_trackdir;
			size_t ramp_index = adapted.size();
			if (!AppendPolyrailLandRun(anchor, detection.last_on_grid_trackdir, height_difference - 1, &adapted, true)) return std::nullopt;
			for (size_t i = ramp_index; i < adapted.size(); i++) {
				Debug(misc, 1, "Polyrail on-grid ramp: start={} end={}",
						FormatPolyrailTile(adapted[i].start), FormatPolyrailTile(adapted[i].end));
			}
		}
	}

	return adapted;
}

static std::optional<std::vector<std::vector<PolyrailSegment>>> BuildPolyrailHeightAdjustedLineRoutes(std::vector<PolyrailLineHeightAnalysis> analyses, const std::vector<Trackdir> &previous_on_grid_trackdirs)
{
	if (analyses.size() != previous_on_grid_trackdirs.size()) return std::nullopt;

	std::optional<uint> shared_prefix_tiles;
	for (const PolyrailLineHeightAnalysis &analysis : analyses) {
		if (!analysis.detection.found) continue;
		shared_prefix_tiles = shared_prefix_tiles.has_value() ? std::min(*shared_prefix_tiles, analysis.detection.prefix_tiles) : analysis.detection.prefix_tiles;
	}

	std::vector<std::vector<PolyrailSegment>> line_routes;
	line_routes.reserve(analyses.size());
	if (!shared_prefix_tiles.has_value() || *shared_prefix_tiles == 0) {
		for (PolyrailLineHeightAnalysis &analysis : analyses) {
			line_routes.push_back(std::move(analysis.route));
		}
		return line_routes;
	}

	for (size_t i = 0; i < analyses.size(); i++) {
		PolyrailHeightDetection detection = analyses[i].detection;
		if (!detection.found) {
			std::optional<PolyrailHeightDetection> fallback = GetPolyrailFallbackHeightDetection(analyses[i].route, previous_on_grid_trackdirs[i]);
			if (!fallback.has_value()) return std::nullopt;
			detection = *fallback;
		}

		std::optional<std::vector<PolyrailSegment>> adapted_route = ApplyPolyrailHeightPrefix(analyses[i], detection, *shared_prefix_tiles);
		if (!adapted_route.has_value() || adapted_route->empty()) return std::nullopt;
		line_routes.push_back(std::move(*adapted_route));
	}
	return line_routes;
}

/* PolyRail start preparation and regular route building. */

static bool MovePolyrailOptimizationStartToBuildTile(PolyrailStart *start)
{
	if (start->tile == INVALID_TILE || start->trackdir == INVALID_TRACKDIR) return false;
	if (!HasPolyrailTrack(start->tile, start->trackdir)) return true;

	start->tile = GetPolyrailOptimizationContinuationTile(start->tile, start->trackdir);
	return start->tile != INVALID_TILE;
}

static bool EnsurePolyrailSecondaryStart(std::vector<PolyrailStart> *starts, TileIndex cursor)
{
	if (starts->size() != POLYRAIL_LINE_COUNT) return false;
	if ((*starts)[POLYRAIL_SECONDARY].tile != INVALID_TILE) return true;

	Track track = TrackdirToTrack((*starts)[POLYRAIL_MAIN].trackdir);
	TileIndexDiffC offset;
	if (!TryGetDefaultPolyrailOffset(track, (*starts)[POLYRAIL_MAIN].tile, cursor, &offset)) return false;

	(*starts)[POLYRAIL_SECONDARY].tile = AddTileIndexDiffCWrap((*starts)[POLYRAIL_MAIN].tile, offset);
	if ((*starts)[POLYRAIL_SECONDARY].tile == INVALID_TILE) return false;

	(*starts)[POLYRAIL_SECONDARY].offset = offset;
	return true;
}

static std::optional<std::vector<PolyrailStart>> PreparePolyrailStartsForDirection(std::vector<PolyrailStart> starts, TileIndex cursor, Direction dir)
{
	if (starts.size() != POLYRAIL_LINE_COUNT) return std::nullopt;

	Trackdir default_trackdir = GetPolyrailTrackdirForDirection(dir);
	if (default_trackdir == INVALID_TRACKDIR) return std::nullopt;

	for (PolyrailStart &start : starts) {
		if (start.trackdir == INVALID_TRACKDIR) {
			start.trackdir = default_trackdir;
			continue;
		}

		Trackdir target = GetPolyrailTargetTrackdir(start.trackdir, dir);
		if (!IsPolyrailTransitionAllowed(start.trackdir, target)) return std::nullopt;
	}

	if (!EnsurePolyrailSecondaryStart(&starts, cursor)) return std::nullopt;

	return starts;
}

static std::optional<std::vector<PolyrailStart>> PreparePolyrailOptimizationStarts(std::vector<PolyrailStart> starts, TileIndex cursor, Direction dir)
{
	if (starts.size() != POLYRAIL_LINE_COUNT) return std::nullopt;

	Trackdir default_trackdir = GetPolyrailTrackdirForDirection(dir);
	if (default_trackdir == INVALID_TRACKDIR) return std::nullopt;

	for (PolyrailStart &start : starts) {
		if (start.trackdir == INVALID_TRACKDIR) start.trackdir = default_trackdir;
	}

	if (!EnsurePolyrailSecondaryStart(&starts, cursor)) return std::nullopt;

	return starts;
}

static std::vector<PolyrailSegment> InterleavePolyrailLineRoutes(const std::vector<std::vector<PolyrailSegment>> &line_routes)
{
	std::vector<PolyrailSegment> route;
	for (const std::vector<PolyrailSegment> &line_route : line_routes) {
		if (line_route.empty()) return {};
		route.reserve(route.size() + line_route.size());
		route.insert(route.end(), line_route.begin(), std::prev(line_route.end()));
	}
	for (const std::vector<PolyrailSegment> &line_route : line_routes) {
		route.push_back(line_route.back());
	}
	return route;
}

static std::vector<PolyrailSegment> BuildPolyrailSegmentsForDirection(const std::vector<PolyrailStart> &starts, TileIndex cursor, Direction dir, bool force_connected_starts = false)
{
	if (starts.size() != POLYRAIL_LINE_COUNT) return {};

	for (const PolyrailStart &start : starts) {
		if (start.trackdir == INVALID_TRACKDIR) return {};
	}

	struct PolyrailBuildLine {
		TileIndex tile = INVALID_TILE;
		Trackdir trackdir = INVALID_TRACKDIR;
		TileIndex endpoint_tile = INVALID_TILE;
		Trackdir endpoint_trackdir = INVALID_TRACKDIR;
		bool can_extend = false;
	};

	auto get_build_line = [&](PolyrailStart start, Direction dir, std::vector<PolyrailSegment> &extensions, PolyrailBuildLine *line) {
		Trackdir target = GetPolyrailTargetTrackdir(start.trackdir, dir);
		if (target == INVALID_TRACKDIR) return false;

		line->tile = start.tile;
		line->trackdir = start.trackdir;
		line->endpoint_tile = start.tile;
		line->endpoint_trackdir = start.trackdir;
		line->can_extend = force_connected_starts || HasPolyrailTrack(start.tile, start.trackdir);
		if (!line->can_extend) return true;

		Trackdir endpoint_trackdir = start.trackdir;
		if (!CanPolyrailTurnStraightToDiagonal(endpoint_trackdir, dir) &&
				!IsDiagonalTrackdir(endpoint_trackdir) &&
				IsDiagonalDirection(dir)) {
			TileIndex extension_tile = AddTileIndexDiffCWrap(start.tile, GetPolyrailTrackdirDelta(endpoint_trackdir));
			if (extension_tile == INVALID_TILE) return false;

			Trackdir extension_trackdir = NextTrackdir(endpoint_trackdir);
			extensions.push_back({extension_tile, extension_tile, TrackdirToTrack(extension_trackdir), extension_trackdir, extension_trackdir});

			start.tile = extension_tile;
			endpoint_trackdir = extension_trackdir;
			target = GetPolyrailTargetTrackdir(endpoint_trackdir, dir);
			if (target == INVALID_TRACKDIR) return false;
		}

		line->tile = AddTileIndexDiffCWrap(start.tile, GetPolyrailTrackdirDelta(endpoint_trackdir));
		if (line->tile == INVALID_TILE) return false;
		line->trackdir = target;
		line->endpoint_tile = start.tile;
		line->endpoint_trackdir = endpoint_trackdir;
		return true;
	};

	auto update_build_line_from_endpoint = [&](PolyrailBuildLine *line) {
		line->tile = AddTileIndexDiffCWrap(line->endpoint_tile, GetPolyrailTrackdirDelta(line->endpoint_trackdir));
		if (line->tile == INVALID_TILE) return false;
		line->trackdir = GetPolyrailTargetTrackdir(line->endpoint_trackdir, dir);
		return line->trackdir != INVALID_TRACKDIR;
	};

	auto extend_build_line = [&](PolyrailBuildLine *line, uint steps, std::vector<PolyrailSegment> &extensions) {
		if (steps == 0) return true;
		if (!line->can_extend) return false;

		TileIndex extension_start = INVALID_TILE;
		Trackdir extension_start_trackdir = INVALID_TRACKDIR;

		for (uint i = 0; i < steps; i++) {
			TileIndex extension_tile = AddTileIndexDiffCWrap(line->endpoint_tile, GetPolyrailTrackdirDelta(line->endpoint_trackdir));
			if (extension_tile == INVALID_TILE) return false;

			Trackdir extension_trackdir = AdvancePolyrailTrackdir(line->endpoint_trackdir);
			if (extension_start == INVALID_TILE) {
				extension_start = extension_tile;
				extension_start_trackdir = extension_trackdir;
			}
			line->endpoint_tile = extension_tile;
			line->endpoint_trackdir = extension_trackdir;
		}

		extensions.push_back({extension_start, line->endpoint_tile, TrackdirToTrack(extension_start_trackdir), extension_start_trackdir, line->endpoint_trackdir});
		return update_build_line_from_endpoint(line);
	};

	auto project_build_lines = [&](const std::vector<PolyrailBuildLine> &candidate_lines, std::vector<PolyrailSegment> *projected, uint64_t *score) {
		uint shared_steps = 0;
		for (size_t i = 0; i < candidate_lines.size(); i++) {
			const PolyrailStart &start = starts[i];
			const PolyrailBuildLine &line = candidate_lines[i];

			TileIndex line_cursor = AddTileIndexDiffCWrap(cursor, start.offset);
			if (line_cursor == INVALID_TILE) return false;

			Trackdir projected_trackdir;
			uint steps;
			ProjectPolyrailEnd(line.tile, line_cursor, line.trackdir, &projected_trackdir, &steps);
			shared_steps = std::max(shared_steps, steps);
		}

		uint64_t candidate_score = 0;
		std::vector<PolyrailSegment> candidate_projected;
		if (projected != nullptr) candidate_projected.reserve(candidate_lines.size());
		for (size_t i = 0; i < candidate_lines.size(); i++) {
			const PolyrailStart &start = starts[i];
			const PolyrailBuildLine &line = candidate_lines[i];

			Trackdir projected_trackdir;
			TileIndex projected_end = ProjectPolyrailEndBySteps(line.tile, line.trackdir, shared_steps, &projected_trackdir);
			if (projected_end == INVALID_TILE) return false;

			TileIndex line_cursor = AddTileIndexDiffCWrap(cursor, start.offset);
			if (line_cursor == INVALID_TILE) return false;

			candidate_score += DistanceManhattan(projected_end, line_cursor);
			if (projected != nullptr) {
					candidate_projected.push_back({line.tile, projected_end, TrackdirToTrack(line.trackdir), line.trackdir, projected_trackdir});
			}
		}

		if (score != nullptr) *score = candidate_score;
		if (projected != nullptr) *projected = std::move(candidate_projected);
		return true;
	};

	std::vector<PolyrailSegment> projected_segments;
	projected_segments.reserve(POLYRAIL_LINE_COUNT);
	std::vector<PolyrailBuildLine> lines;
	lines.reserve(POLYRAIL_LINE_COUNT);
	std::vector<std::vector<PolyrailSegment>> line_routes;
	line_routes.reserve(POLYRAIL_LINE_COUNT);
	for (const PolyrailStart &start : starts) {
		PolyrailBuildLine line;
		std::vector<PolyrailSegment> line_route;
		if (!get_build_line(start, dir, line_route, &line)) return {};
		lines.push_back(line);
		line_routes.push_back(std::move(line_route));
	}

	if (IsDiagonalDirection(dir) && lines.size() == POLYRAIL_LINE_COUNT) {
		Track track = TrackdirToTrack(lines[POLYRAIL_MAIN].trackdir);
		bool compare_x = track == TRACK_Y;
		bool compare_y = track == TRACK_X;
		if ((compare_x || compare_y) && TrackdirToTrack(lines[POLYRAIL_SECONDARY].trackdir) == track) {
			bool same_lane = compare_x ?
					TileX(lines[POLYRAIL_MAIN].tile) == TileX(lines[POLYRAIL_SECONDARY].tile) :
					TileY(lines[POLYRAIL_MAIN].tile) == TileY(lines[POLYRAIL_SECONDARY].tile);
			if (same_lane) {
				uint main_distance = DistanceManhattan(lines[POLYRAIL_MAIN].tile, cursor);
				uint secondary_distance = DistanceManhattan(lines[POLYRAIL_SECONDARY].tile, cursor);
				size_t outer = main_distance >= secondary_distance ? POLYRAIL_MAIN : POLYRAIL_SECONDARY;
				if (lines[outer].can_extend && !extend_build_line(&lines[outer], 2, line_routes[outer])) return {};
			}
		}
	}

	uint64_t best_score = UINT64_MAX;
	uint best_extra_steps = 0;
	uint max_extra_steps = DistanceManhattan(lines[POLYRAIL_MAIN].endpoint_tile, cursor) + 4;
	for (uint extra_steps = 0; extra_steps < max_extra_steps; extra_steps += 2) {
		std::vector<PolyrailBuildLine> candidate_lines = lines;
		std::vector<PolyrailSegment> candidate_extensions;
		candidate_extensions.reserve(candidate_lines.size());

		bool valid = true;
		for (PolyrailBuildLine &line : candidate_lines) {
			if (!extend_build_line(&line, extra_steps, candidate_extensions)) {
				valid = false;
				break;
			}
		}
		if (!valid) break;

		uint64_t candidate_score;
		if (!project_build_lines(candidate_lines, nullptr, &candidate_score)) break;

		if (candidate_score < best_score) {
			best_score = candidate_score;
			best_extra_steps = extra_steps;
		}
	}

	for (size_t i = 0; i < lines.size(); i++) {
		if (!extend_build_line(&lines[i], best_extra_steps, line_routes[i])) return {};
	}
	if (!project_build_lines(lines, &projected_segments, nullptr)) return {};
	if (projected_segments.size() != line_routes.size()) return {};

	for (size_t i = 0; i < projected_segments.size(); i++) {
		line_routes[i].push_back(projected_segments[i]);
	}

	std::vector<PolyrailLineHeightAnalysis> line_analyses;
	std::vector<Trackdir> previous_on_grid_trackdirs;
	line_analyses.reserve(line_routes.size());
	previous_on_grid_trackdirs.reserve(line_routes.size());
	for (size_t i = 0; i < line_routes.size(); i++) {
		Trackdir previous_on_grid_trackdir = IsPolyrailOnGridTrack(TrackdirToTrack(starts[i].trackdir)) ? starts[i].trackdir : INVALID_TRACKDIR;
		std::optional<PolyrailLineHeightAnalysis> analysis = AnalyzePolyrailLineRouteForHeight(std::move(line_routes[i]), previous_on_grid_trackdir);
		if (!analysis.has_value() || analysis->route.empty()) return {};

		line_analyses.push_back(std::move(*analysis));
		previous_on_grid_trackdirs.push_back(previous_on_grid_trackdir);
	}
	std::optional<std::vector<std::vector<PolyrailSegment>>> synchronized_routes = BuildPolyrailHeightAdjustedLineRoutes(std::move(line_analyses), previous_on_grid_trackdirs);
	if (!synchronized_routes.has_value()) return {};
	line_routes = std::move(*synchronized_routes);

	return InterleavePolyrailLineRoutes(line_routes);
}

static std::vector<PolyrailSegment> BuildPolyrailRouteWithDirection(const std::vector<PolyrailStart> &starts, TileIndex cursor, Direction dir, bool force_connected_starts = false)
{
	if (starts.size() != POLYRAIL_LINE_COUNT || dir == INVALID_DIR) return {};

	std::optional<std::vector<PolyrailStart>> prepared_starts = PreparePolyrailStartsForDirection(starts, cursor, dir);
	if (!prepared_starts.has_value()) return {};

	return BuildPolyrailSegmentsForDirection(*prepared_starts, cursor, dir, force_connected_starts);
}

static std::vector<PolyrailSegment> BuildPolyrailRoute(const std::vector<PolyrailStart> &starts, TileIndex cursor)
{
	if (starts.size() != POLYRAIL_LINE_COUNT) return {};

	Direction dir = ChoosePolyrailDirection(starts[POLYRAIL_MAIN].tile, cursor, starts[POLYRAIL_MAIN].trackdir);
	return BuildPolyrailRouteWithDirection(starts, cursor, dir);
}

/* PolyRail Optimization route building. */

static std::optional<std::vector<PolyrailOptimizationSegment>> BuildPolyrailOptimizationLineSegments(const PolyrailStart &start, const PolyrailStart &end)
{
	if (start.tile == INVALID_TILE || start.trackdir == INVALID_TRACKDIR || end.tile == INVALID_TILE || end.trackdir == INVALID_TRACKDIR) return std::nullopt;

	Direction start_dir = GetPolyrailTrackdirDirection(start.trackdir);
	Direction end_dir = GetPolyrailTrackdirDirection(end.trackdir);
	std::optional<std::vector<Direction>> directions = BuildDirectionSequence(start_dir, end_dir, start.tile, end.tile);
	if (!directions.has_value() || directions->empty()) return std::nullopt;

	std::optional<std::vector<PolyrailOptimizationSegment>> segments = BuildPolyrailOptimizationSegments(*directions, start.tile, end.tile);
	if (!segments.has_value() || segments->empty()) return std::nullopt;
	if (!OptimizePolyrailOptimizationSegmentLengths(start, end, *segments)) return std::nullopt;

	return segments;
}

static std::optional<std::vector<PolyrailSegment>> BuildPolyrailOptimizationLineRoute(const PolyrailStart &start, const std::vector<PolyrailOptimizationSegment> &segments)
{
	if (start.tile == INVALID_TILE || start.trackdir == INVALID_TRACKDIR || segments.empty()) return std::nullopt;

	std::vector<PolyrailSegment> line_route;
	line_route.reserve(segments.size());

	TileIndex segment_start = start.tile;
	Trackdir previous_trackdir = start.trackdir;
	for (size_t i = 0; i < segments.size(); i++) {
		const PolyrailOptimizationSegment &segment = segments[i];
		Trackdir trackdir = GetPolyrailOptimizationSegmentTrackdir(previous_trackdir, segment.dir);
		if (trackdir == INVALID_TRACKDIR) return std::nullopt;

		Trackdir end_trackdir = INVALID_TRACKDIR;
		TileIndex segment_end = ProjectPolyrailEndBySteps(segment_start, trackdir, segment.length, &end_trackdir);
		if (segment_end == INVALID_TILE) return std::nullopt;

		line_route.push_back({segment_start, segment_end, TrackdirToTrack(trackdir), trackdir, end_trackdir});
		previous_trackdir = end_trackdir;
		if (i + 1 != segments.size()) {
			segment_start = GetPolyrailOptimizationContinuationTile(segment_end, end_trackdir);
			if (segment_start == INVALID_TILE) return std::nullopt;
		}
	}

	return line_route;
}

static std::optional<std::vector<PolyrailEqualizationCommand>> BuildPolyrailLevelLandCommandsForLineRoutes(const std::vector<std::vector<PolyrailSegment>> &line_routes);


static std::optional<std::vector<std::vector<PolyrailSegment>>> BuildPolyrailOptimizationLineRoutes(const PolyrailOptimizationRouteSettings &settings)
{
	if (settings.starts.size() != POLYRAIL_LINE_COUNT || settings.ends.size() != POLYRAIL_LINE_COUNT) return std::nullopt;

	std::vector<PolyrailStart> starts = settings.starts;
	for (PolyrailStart &start : starts) {
		if (!MovePolyrailOptimizationStartToBuildTile(&start)) return std::nullopt;
	}

	std::vector<PolyrailLineHeightAnalysis> line_analyses;
	std::vector<Trackdir> previous_on_grid_trackdirs;
	line_analyses.reserve(starts.size());
	previous_on_grid_trackdirs.reserve(starts.size());
	for (size_t i = 0; i < starts.size(); i++) {
		std::optional<std::vector<PolyrailOptimizationSegment>> segments = BuildPolyrailOptimizationLineSegments(starts[i], settings.ends[i]);
		if (!segments.has_value()) return std::nullopt;

		std::optional<std::vector<PolyrailSegment>> line_route = BuildPolyrailOptimizationLineRoute(starts[i], *segments);
		if (!line_route.has_value()) return std::nullopt;

		Trackdir previous_on_grid_trackdir = IsPolyrailOnGridTrack(TrackdirToTrack(starts[i].trackdir)) ? starts[i].trackdir : INVALID_TRACKDIR;
		std::optional<PolyrailLineHeightAnalysis> analysis = AnalyzePolyrailLineRouteForHeight(std::move(*line_route), previous_on_grid_trackdir);
		if (!analysis.has_value() || analysis->route.empty()) return std::nullopt;

		line_analyses.push_back(std::move(*analysis));
		previous_on_grid_trackdirs.push_back(previous_on_grid_trackdir);
	}

	return BuildPolyrailHeightAdjustedLineRoutes(std::move(line_analyses), previous_on_grid_trackdirs);
}

static std::vector<PolyrailSegment> BuildPolyrailOptimizationRoute(const PolyrailOptimizationRouteSettings &settings)
{
	std::optional<std::vector<std::vector<PolyrailSegment>>> line_routes = BuildPolyrailOptimizationLineRoutes(settings);
	if (!line_routes.has_value()) return {};
	return InterleavePolyrailLineRoutes(*line_routes);
}

/* PolyRail Optimization bridge-obstacle build planning. */

static bool AppendPolyrailLandRun(TileIndex start, Trackdir trackdir, uint steps, std::vector<PolyrailSegment> *rails, bool include_single_tile = false)
{
	if (start == INVALID_TILE || trackdir == INVALID_TRACKDIR) return false;
	if (steps == 0) {
		if (include_single_tile) rails->push_back({start, start, TrackdirToTrack(trackdir), trackdir, trackdir});
		return true;
	}

	Trackdir end_trackdir = INVALID_TRACKDIR;
	TileIndex end = ProjectPolyrailEndBySteps(start, trackdir, steps, &end_trackdir);
	if (end == INVALID_TILE || end_trackdir == INVALID_TRACKDIR) return false;

	rails->push_back({start, end, TrackdirToTrack(trackdir), trackdir, end_trackdir});
	return true;
}

static bool TryBuildPolyrailOptimizationBridge(TileIndex start, TileIndex end, PolyrailOptimizationBridge *bridge)
{
	if (start == INVALID_TILE || end == INVALID_TILE || bridge == nullptr) return false;

	Track track;
	if (TileY(start) == TileY(end)) {
		track = TRACK_X;
	} else if (TileX(start) == TileX(end)) {
		track = TRACK_Y;
	} else {
		return false;
	}

	uint bridge_len = GetTunnelBridgeLength(start, end);
	BridgeType bridge_type;
	if (!GetPreferredRailBridgeType(bridge_len, &bridge_type)) return false;

	CommandCost ret = Command<Commands::BuildBridge>::Do(
			CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildBridge>()) | DoCommandFlag::QueryCost,
			end, start, TRANSPORT_RAIL, bridge_type, _cur_railtype, INVALID_ROADTYPE);
	if (ret.Failed()) return false;

	*bridge = {start, end, bridge_type, track};
	return true;
}

static Track GetPolyrailOptimizationBridgeTrack(TileIndex from, TileIndex to)
{
	if (from == INVALID_TILE || to == INVALID_TILE) return INVALID_TRACK;
	if (TileY(from) == TileY(to)) return TRACK_X;
	if (TileX(from) == TileX(to)) return TRACK_Y;
	return INVALID_TRACK;
}

static bool IsPolyrailOptimizationRailBridgeObstacle(TileIndex tile, Track bridge_track)
{
	if (tile == INVALID_TILE || !IsPlainRailTile(tile)) return false;
	if (bridge_track != TRACK_X && bridge_track != TRACK_Y) return false;
	return (GetTrackBits(tile) & TrackCrossesTracks(bridge_track)) != 0;
}

static bool IsPolyrailOptimizationBridgeObstacle(TileIndex tile, Track bridge_track)
{
	return IsWaterTile(tile) || IsPolyrailOptimizationRailBridgeObstacle(tile, bridge_track);
}

static bool IsPolyrailBridgeableOnGridTrack(Track track)
{
	return track == TRACK_X || track == TRACK_Y;
}

struct PolyrailOptimizationBridgeSpan {
	TileIndex end = INVALID_TILE;
	Trackdir trackdir = INVALID_TRACKDIR;
};

static bool FindPolyrailOptimizationBridgeSpan(TileIndex segment_end, Track bridge_track, TileIndex obstacle_tile, Trackdir obstacle_trackdir, PolyrailOptimizationBridgeSpan *span)
{
	while (obstacle_tile != segment_end && IsPolyrailOptimizationBridgeObstacle(obstacle_tile, bridge_track)) {
		TileIndex after_obstacle = StepPolyrailTrackdir(obstacle_tile, &obstacle_trackdir);
		if (after_obstacle == INVALID_TILE) return false;

		obstacle_tile = after_obstacle;
	}

	if (IsPolyrailOptimizationBridgeObstacle(obstacle_tile, bridge_track)) return false;

	span->end = obstacle_tile;
	span->trackdir = obstacle_trackdir;
	return true;
}

static bool SplitPolyrailSegmentForBridgeObstacles(const PolyrailSegment &segment, bool require_on_grid_segment, std::vector<PolyrailSegment> *rails, std::vector<PolyrailOptimizationBridge> *bridges, bool *found_obstacle)
{
	if (segment.start == segment.end) {
		rails->push_back(segment);
		return true;
	}

	if (require_on_grid_segment && !IsPolyrailBridgeableOnGridTrack(segment.track)) {
		rails->push_back(segment);
		return true;
	}

	Trackdir trackdir = segment.start_trackdir;
	if (trackdir == INVALID_TRACKDIR) return false;

	TileIndex tile = segment.start;
	TileIndex run_start = tile;
	Trackdir run_trackdir = trackdir;
	uint run_steps = 0;
	bool segment_found_obstacle = false;

	for (uint guard = 0; tile != segment.end && guard < Map::Size(); guard++) {
		TileIndex next = AddTileIndexDiffCWrap(tile, GetPolyrailTrackdirDelta(trackdir));
		if (next == INVALID_TILE) return false;

		Trackdir next_trackdir = trackdir;
		if (!IsDiagonalTrackdir(next_trackdir)) ToggleBit(next_trackdir, 0);

		Track bridge_track = GetPolyrailOptimizationBridgeTrack(tile, next);
		if (!IsPolyrailOptimizationBridgeObstacle(next, bridge_track)) {
			tile = next;
			trackdir = next_trackdir;
			run_steps++;
			continue;
		}

		*found_obstacle = true;
		segment_found_obstacle = true;
		if (IsPolyrailOptimizationBridgeObstacle(tile, bridge_track)) return false;

		uint rail_steps_before_ramp = run_steps > 0 ? run_steps - 1 : 0;
		if (!AppendPolyrailLandRun(run_start, run_trackdir, rail_steps_before_ramp, rails, run_start != tile)) return false;

		TileIndex bridge_start = tile;
		PolyrailOptimizationBridgeSpan span;
		if (!FindPolyrailOptimizationBridgeSpan(segment.end, bridge_track, next, next_trackdir, &span)) return false;

		PolyrailOptimizationBridge bridge;
		if (!TryBuildPolyrailOptimizationBridge(bridge_start, span.end, &bridge)) return false;
		bridges->push_back(bridge);

		tile = span.end;
		trackdir = span.trackdir;
		if (tile == segment.end) {
			run_start = INVALID_TILE;
			run_steps = 0;
			break;
		}

		TileIndex after_ramp = AddTileIndexDiffCWrap(tile, GetPolyrailTrackdirDelta(trackdir));
		Track after_ramp_bridge_track = GetPolyrailOptimizationBridgeTrack(tile, after_ramp);
		if (after_ramp == INVALID_TILE || IsPolyrailOptimizationBridgeObstacle(after_ramp, after_ramp_bridge_track)) return false;

		Trackdir after_ramp_trackdir = trackdir;
		if (!IsDiagonalTrackdir(after_ramp_trackdir)) ToggleBit(after_ramp_trackdir, 0);

		tile = after_ramp;
		trackdir = after_ramp_trackdir;
		run_start = tile;
		run_trackdir = trackdir;
		run_steps = 0;
	}

	if (tile != segment.end) return false;
	if (run_start == INVALID_TILE) return true;
	return AppendPolyrailLandRun(run_start, run_trackdir, run_steps, rails, segment_found_obstacle);
}

static bool TestPolyrailTerraformCommand(const PolyrailTerraformCommand &terraform)
{
	if (terraform.tile == INVALID_TILE || terraform.slope == SLOPE_FLAT) return false;

	CommandCost ret;
	std::tie(ret, std::ignore, std::ignore) = Command<Commands::TerraformLand>::Do(
			CommandFlagsToDCFlags(GetCommandFlags<Commands::TerraformLand>()) | DoCommandFlag::QueryCost,
			terraform.tile, terraform.slope, terraform.dir_up);
	return ret.Succeeded();
}

static bool AppendPolyrailTerraformCommand(TileIndex tile, Slope slope, bool dir_up, std::vector<PolyrailTerraformCommand> *terraforms)
{
	if (slope == SLOPE_FLAT) return true;

	PolyrailTerraformCommand terraform{tile, slope, dir_up};
	if (!TestPolyrailTerraformCommand(terraform)) return false;

	terraforms->push_back(terraform);
	return true;
}

static bool AppendPolyrailHeightRampTerraformCommands(TileIndex tile, Slope desired_slope, std::vector<PolyrailTerraformCommand> *terraforms)
{
	Slope cur_slope = RemoveHalftileSlope(GetTileSlope(tile));
	if (cur_slope == desired_slope) return true;
	if (IsSteepSlope(cur_slope)) return false;

	Slope to_lower = cur_slope & ComplementSlope(desired_slope);
	Slope remaining = cur_slope & desired_slope;
	Slope to_raise = desired_slope & ComplementSlope(remaining);

	return AppendPolyrailTerraformCommand(tile, to_lower, false, terraforms) &&
			AppendPolyrailTerraformCommand(tile, to_raise, true, terraforms);
}

static bool AppendPolyrailHeightRampTerraformCommands(TileIndex start, Trackdir trackdir, std::vector<PolyrailTerraformCommand> *terraforms)
{
	if (start == INVALID_TILE || trackdir == INVALID_TRACKDIR || !IsPolyrailOnGridTrack(TrackdirToTrack(trackdir))) return false;

	std::optional<PolyrailHeightTarget> target = FindPolyrailHeightRampTarget(start, trackdir);
	if (!target.has_value()) return false;

	int height_delta = target->reference_height - GetTileZ(start);
	if (height_delta == 0) return true;

	DiagDirection slope_dir = DirToDiagDir(GetPolyrailTrackdirDirection(trackdir));
	if (height_delta < 0) slope_dir = ReverseDiagDir(slope_dir);
	Slope desired_slope = InclinedSlope(slope_dir);

	uint ramp_tiles = static_cast<uint>(std::abs(height_delta));
	TileIndex tile = start;
	for (uint guard = 0; guard < ramp_tiles; guard++) {
		if (!AppendPolyrailHeightRampTerraformCommands(tile, desired_slope, terraforms)) return false;
		if (guard + 1 == ramp_tiles) return true;

		tile = StepPolyrailTrackdir(tile, &trackdir);
		if (tile == INVALID_TILE) return false;
	}

	return false;
}

static bool BuildPolyrailTerraformCommandsFromRoute(const std::vector<PolyrailSegment> &route, std::vector<PolyrailTerraformCommand> *terraforms)
{
	for (const PolyrailSegment &segment : route) {
		if (segment.height_ramp_start == INVALID_TILE) continue;
		if (!AppendPolyrailHeightRampTerraformCommands(segment.height_ramp_start, segment.height_ramp_trackdir, terraforms)) return false;
	}

	return true;
}

enum class PolyrailEqualizationValidity {
	Valid,
	AlreadyLevelled,
	Invalid,
};

static PolyrailEqualizationValidity TestPolyrailEqualizationCommand(const PolyrailEqualizationCommand &equalization)
{
	if (equalization.start_tile == INVALID_TILE || equalization.end_tile == INVALID_TILE) return PolyrailEqualizationValidity::Invalid;

	CommandCost ret;
	std::tie(ret, std::ignore, std::ignore) = Command<Commands::LevelLand>::Do(
			CommandFlagsToDCFlags(GetCommandFlags<Commands::LevelLand>()) | DoCommandFlag::QueryCost,
			equalization.end_tile, equalization.start_tile, false, LM_LEVEL_FLAT);
	if (ret.Succeeded()) return PolyrailEqualizationValidity::Valid;
	if (ret.GetErrorMessage() == STR_ERROR_ALREADY_LEVELLED) return PolyrailEqualizationValidity::AlreadyLevelled;
	return PolyrailEqualizationValidity::Invalid;
}

static void AppendPolyrailTileCornerIndices(TileIndex tile, std::vector<TileIndex> *corners)
{
	if (tile == INVALID_TILE || tile >= Map::Size()) return;

	uint x = TileX(tile);
	uint y = TileY(tile);
	corners->push_back(TileXY(x, y));
	corners->push_back(TileXY(std::min(x + 1, Map::MaxX()), y));
	corners->push_back(TileXY(x, std::min(y + 1, Map::MaxY())));
	corners->push_back(TileXY(std::min(x + 1, Map::MaxX()), std::min(y + 1, Map::MaxY())));
}

static std::optional<PolyrailLevelLandArea> FindPolyrailLevelLandArea(TileIndex start_tile, TileIndex end_tile)
{
	std::vector<TileIndex> corners;
	corners.reserve(8);
	AppendPolyrailTileCornerIndices(start_tile, &corners);
	AppendPolyrailTileCornerIndices(end_tile, &corners);
	if (corners.size() < 2) return std::nullopt;

	PolyrailLevelLandArea best;
	uint best_distance = 0;
	for (size_t i = 0; i < corners.size(); i++) {
		for (size_t j = i + 1; j < corners.size(); j++) {
			uint distance = DistanceManhattan(corners[i], corners[j]);
			if (best.start_index == INVALID_TILE || distance > best_distance) {
				best = {corners[i], corners[j]};
				best_distance = distance;
			}
		}
	}

	return best.start_index != INVALID_TILE && best.end_index != INVALID_TILE ? std::optional<PolyrailLevelLandArea>{best} : std::nullopt;
}

static std::optional<PolyrailEqualizationCommand> BuildPolyrailLevelLandCommand(TileIndex start_tile, TileIndex end_tile)
{
	std::optional<PolyrailLevelLandArea> area = FindPolyrailLevelLandArea(start_tile, end_tile);
	if (!area.has_value()) return std::nullopt;

	return PolyrailEqualizationCommand{area->start_index, area->end_index, true};
}

static bool HasPolyrailHeightReferenceTile(const PolyrailSegment &segment)
{
	return segment.height_reference_tile != INVALID_TILE;
}

static std::optional<PolyrailEqualizationCommand> BuildPolyrailLevelLandCommandForSegmentPair(const PolyrailSegment &first, const PolyrailSegment &second)
{
	auto tile_log = [](TileIndex tile) {
		if (tile == INVALID_TILE) return std::string{"INVALID_TILE"};
		if (tile >= Map::Size()) return fmt::format("OUT_OF_MAP({})", tile.base());
		return fmt::format("({}, {})", TileX(tile), TileY(tile));
	};

	if (first.end == INVALID_TILE || second.end == INVALID_TILE) return std::nullopt;

	const PolyrailSegment *start_segment = &first;
	const PolyrailSegment *end_segment = &second;
	uint first_distance = DistanceManhattan(first.end, first.height_reference_tile);
	uint second_distance = DistanceManhattan(second.end, second.height_reference_tile);
	if (second_distance > first_distance) std::swap(start_segment, end_segment);

	if (end_segment->height_last_on_grid_trackdir == INVALID_TRACKDIR) return std::nullopt;

	TileIndex start_tile = start_segment->height_reference_tile;
	uint height_delta = static_cast<uint>(std::abs(start_segment->height_reference - GetTileZ(start_segment->end)));

	Trackdir end_trackdir = INVALID_TRACKDIR;
	TileIndex end_tile = ProjectPolyrailEndBySteps(end_segment->end, end_segment->height_last_on_grid_trackdir, height_delta + 2, &end_trackdir);
	if (end_tile == INVALID_TILE) return std::nullopt;

	int start_height = start_segment->height_reference;
	int end_height = end_tile != INVALID_TILE && end_tile < Map::Size() ? GetTileZ(end_tile) : 0;
	Debug(misc, 1, "Polyrail level-land: height_reference_tile={} end_tile={} start_h={} end_h={} height_delta={}",
			tile_log(start_tile), tile_log(end_tile), start_height, end_height, height_delta);
	Debug(misc, 1, "Polyrail level-land: start_seg1={} start_seg2={} end_seg1={} end_seg2={}",
			tile_log(first.start), tile_log(second.start), tile_log(first.end), tile_log(second.end));

	return BuildPolyrailLevelLandCommand(start_tile, end_tile);
}

static std::optional<std::vector<PolyrailEqualizationCommand>> BuildPolyrailLevelLandCommandsForLineRoutes(const std::vector<std::vector<PolyrailSegment>> &line_routes)
{
	if (line_routes.size() != POLYRAIL_LINE_COUNT) return std::nullopt;

	const std::vector<PolyrailSegment> &first_route = line_routes[POLYRAIL_MAIN];
	const std::vector<PolyrailSegment> &second_route = line_routes[POLYRAIL_SECONDARY];
	if (first_route.size() != second_route.size()) {
		Debug(misc, 1, "Polyrail level-land: line route length mismatch: first_route_segments={} second_route_segments={}",
				first_route.size(), second_route.size());
		return std::nullopt;
	}

	std::vector<PolyrailEqualizationCommand> commands;
	for (size_t i = 0; i < first_route.size(); i++) {
		if (!HasPolyrailHeightReferenceTile(first_route[i]) || !HasPolyrailHeightReferenceTile(second_route[i])) continue;

		std::optional<PolyrailEqualizationCommand> command = BuildPolyrailLevelLandCommandForSegmentPair(first_route[i], second_route[i]);
		if (command.has_value()) commands.push_back(*command);
	}

	return commands;
}

static std::optional<std::vector<std::vector<PolyrailSegment>>> DeinterleavePolyrailLineRoutes(const std::vector<PolyrailSegment> &route)
{
	if (route.size() < POLYRAIL_LINE_COUNT || route.size() % POLYRAIL_LINE_COUNT != 0) {
		Debug(misc, 1, "Polyrail level-land: cannot split route into two equal-length lines: route_segments={}", route.size());
		return std::nullopt;
	}

	size_t segment_count = route.size() / POLYRAIL_LINE_COUNT;
	std::vector<std::vector<PolyrailSegment>> line_routes(POLYRAIL_LINE_COUNT);
	for (std::vector<PolyrailSegment> &line_route : line_routes) {
		line_route.reserve(segment_count);
	}

	size_t route_index = 0;
	for (size_t line = 0; line < POLYRAIL_LINE_COUNT; line++) {
		for (size_t segment = 0; segment + 1 < segment_count; segment++) {
			line_routes[line].push_back(route[route_index++]);
		}
	}
	for (size_t line = 0; line < POLYRAIL_LINE_COUNT; line++) {
		line_routes[line].push_back(route[route_index++]);
	}

	return line_routes;
}

struct PolyrailHeightReference {
	TileIndex reference_tile = INVALID_TILE;
	int reference_height = 0;
	TileIndex anchor_tile = INVALID_TILE;
	TileIndex prefix_end_tile = INVALID_TILE;
	Trackdir last_on_grid_trackdir = INVALID_TRACKDIR;
};

static bool TryGetPolyrailHeightAdjustment(const PolyrailSegment &segment, PolyrailHeightReference *reference)
{
	if (segment.height_prefix_end_tile == INVALID_TILE || segment.height_last_on_grid_trackdir == INVALID_TRACKDIR) return false;

	*reference = {
		segment.height_reference_tile,
		segment.height_reference,
		segment.height_anchor_tile,
		segment.height_prefix_end_tile,
		segment.height_last_on_grid_trackdir,
	};
	return true;
}

static bool IsPolyrailHeightReferenceValid(const PolyrailHeightReference &reference)
{
	return reference.reference_tile != INVALID_TILE;
}

static uint GetPolyrailHeightReferenceChange(const PolyrailHeightReference &reference)
{
	return static_cast<uint>(std::abs(reference.reference_height - GetTileZ(reference.anchor_tile)));
}

static bool AppendPolyrailEqualizationCommandFromRoute(const std::vector<PolyrailSegment> &route, std::vector<PolyrailEqualizationCommand> *equalizations)
{
	std::vector<PolyrailHeightReference> references;
	std::vector<PolyrailHeightReference> adjustments;
	references.reserve(POLYRAIL_LINE_COUNT);
	adjustments.reserve(POLYRAIL_LINE_COUNT);
	for (const PolyrailSegment &segment : route) {
		PolyrailHeightReference reference;
		if (!TryGetPolyrailHeightAdjustment(segment, &reference)) continue;

		adjustments.push_back(reference);
		if (IsPolyrailHeightReferenceValid(reference)) references.push_back(reference);
	}

	if (references.empty()) return true;

	size_t chosen = POLYRAIL_MAIN;
	if (references.size() >= POLYRAIL_LINE_COUNT) {
		chosen = GetPolyrailHeightReferenceChange(references[POLYRAIL_SECONDARY]) > GetPolyrailHeightReferenceChange(references[POLYRAIL_MAIN]) ?
				POLYRAIL_SECONDARY : POLYRAIL_MAIN;
	}

	PolyrailHeightReference end_reference = references[chosen];
	for (const PolyrailHeightReference &adjustment : adjustments) {
		if (adjustment.prefix_end_tile != references[chosen].prefix_end_tile ||
				adjustment.last_on_grid_trackdir != references[chosen].last_on_grid_trackdir) {
			end_reference = adjustment;
			break;
		}
	}

	Trackdir end_trackdir = INVALID_TRACKDIR;
	TileIndex end_tile = ProjectPolyrailEndBySteps(end_reference.prefix_end_tile, end_reference.last_on_grid_trackdir, 2, &end_trackdir);
	if (end_tile == INVALID_TILE) return false;

	PolyrailEqualizationCommand command{references[chosen].reference_tile, end_tile, true};
	PolyrailEqualizationValidity validity = TestPolyrailEqualizationCommand(command);
	if (validity == PolyrailEqualizationValidity::Invalid) return false;
	if (validity == PolyrailEqualizationValidity::AlreadyLevelled) command.post_level = false;

	equalizations->push_back(command);
	return true;
}

static std::optional<PolyrailOptimizationBuildPlan> BuildPolyrailBuildPlanFromRoute(std::vector<PolyrailSegment> route, bool require_on_grid_segments, bool apply_height_commands)
{
	if (route.empty()) return std::nullopt;

	PolyrailOptimizationBuildPlan plan;
	plan.final_route = route;
	if (apply_height_commands) {
		if (!BuildPolyrailTerraformCommandsFromRoute(route, &plan.terraforms)) return std::nullopt;
		if (!AppendPolyrailEqualizationCommandFromRoute(route, &plan.equalizations)) return std::nullopt;
	}

	bool found_obstacle = false;
	for (const PolyrailSegment &segment : route) {
		if (!SplitPolyrailSegmentForBridgeObstacles(segment, require_on_grid_segments, &plan.rails, &plan.bridges, &found_obstacle)) return std::nullopt;
	}

	if (!found_obstacle) {
		plan.rails = std::move(route);
		plan.bridges.clear();
	}

	return plan;
}

static std::optional<PolyrailOptimizationBuildPlan> BuildPolyrailOptimizationBuildPlanFromRoute(std::vector<PolyrailSegment> route)
{
	return BuildPolyrailBuildPlanFromRoute(std::move(route), false, false);
}

static std::optional<PolyrailOptimizationBuildPlan> BuildPolyrailOptimizationBuildPlanFromLineRoutes(const std::vector<std::vector<PolyrailSegment>> &line_routes)
{
	std::optional<std::vector<PolyrailEqualizationCommand>> equalizations = BuildPolyrailLevelLandCommandsForLineRoutes(line_routes);
	if (!equalizations.has_value()) return std::nullopt;

	std::optional<PolyrailOptimizationBuildPlan> plan = BuildPolyrailOptimizationBuildPlanFromRoute(InterleavePolyrailLineRoutes(line_routes));
	if (!plan.has_value()) return std::nullopt;

	plan->equalizations = std::move(*equalizations);
	return plan;
}

static std::optional<PolyrailOptimizationBuildPlan> BuildPolyrailOptimizationBuildPlanFromInterleavedRoute(const std::vector<PolyrailSegment> &route)
{
	std::optional<std::vector<std::vector<PolyrailSegment>>> line_routes = DeinterleavePolyrailLineRoutes(route);
	return line_routes.has_value() ? BuildPolyrailOptimizationBuildPlanFromLineRoutes(*line_routes) : std::nullopt;
}

static std::optional<PolyrailOptimizationBuildPlan> BuildPolyrailRegularBuildPlanFromRoute(std::vector<PolyrailSegment> route)
{
	return BuildPolyrailBuildPlanFromRoute(std::move(route), true, true);
}

static std::optional<PolyrailOptimizationBuildPlan> BuildPolyrailOptimizationBuildPlan(const PolyrailOptimizationRouteSettings &settings)
{
	std::optional<std::vector<std::vector<PolyrailSegment>>> line_routes = BuildPolyrailOptimizationLineRoutes(settings);
	return line_routes.has_value() ? BuildPolyrailOptimizationBuildPlanFromLineRoutes(*line_routes) : std::nullopt;
}

/* PolyRail command posting and loose-end selection. */

static bool TryGetDefaultPolyrailOffset(Track track, TileIndex start, TileIndex end, TileIndexDiffC *offset)
{
	static const TileIndexDiffC preferred_offsets[TRACK_END] = {
		{ 0,  1}, // TRACK_X
		{ 1,  0}, // TRACK_Y
		{ 0,  1}, // TRACK_UPPER
		{ 0, -1}, // TRACK_LOWER
		{ 1,  0}, // TRACK_LEFT
		{-1,  0}, // TRACK_RIGHT
	};

	auto valid_offset = [&](TileIndexDiffC candidate) {
		return AddTileIndexDiffCWrap(start, candidate) != INVALID_TILE && AddTileIndexDiffCWrap(end, candidate) != INVALID_TILE;
	};

	TileIndexDiffC candidate = preferred_offsets[track];
	if (valid_offset(candidate)) {
		*offset = candidate;
		return true;
	}

	candidate = {static_cast<int16_t>(-candidate.x), static_cast<int16_t>(-candidate.y)};
	if (valid_offset(candidate)) {
		*offset = candidate;
		return true;
	}

	return false;
}

static void BuildPolyrailRouteCommands(const std::vector<PolyrailSegment> &route)
{
	for (const PolyrailSegment &segment : route) {
		PostPolyrailTrackCommand(segment.start, segment.end, segment.track);
	}
}

static void PostPolyrailTerraformCommand(const PolyrailTerraformCommand &terraform)
{
	Command<Commands::TerraformLand>::Post(
			terraform.dir_up ? STR_ERROR_CAN_T_RAISE_LAND_HERE : STR_ERROR_CAN_T_LOWER_LAND_HERE,
			CcTerraform, terraform.tile, terraform.slope, terraform.dir_up);
}

static void PostPolyrailEqualizationCommand(const PolyrailEqualizationCommand &equalization)
{
	Command<Commands::LandscapeClear>::Post(equalization.start_tile);
	if (equalization.post_level) {
		Command<Commands::LevelLand>::Post(STR_ERROR_CAN_T_LEVEL_LAND_HERE, CcTerraform,
				equalization.end_tile, equalization.start_tile, false, LM_LEVEL_FLAT);
	}
}

static void PostPolyrailEqualizationCommands(const std::vector<PolyrailEqualizationCommand> &equalizations)
{
	for (const PolyrailEqualizationCommand &equalization : equalizations) {
		PostPolyrailEqualizationCommand(equalization);
	}
}

static void PostPolyrailOptimizationBridgeCommand(const PolyrailOptimizationBridge &bridge)
{
	Command<Commands::BuildBridge>::Post(STR_ERROR_CAN_T_BUILD_BRIDGE_HERE, CcBuildBridge,
			bridge.end, bridge.start, TRANSPORT_RAIL, bridge.bridge_type, _cur_railtype, INVALID_ROADTYPE);
}

static void PostPolyrailOptimizationBuildPlan(const PolyrailOptimizationBuildPlan &plan)
{
	PostPolyrailEqualizationCommands(plan.equalizations);
	for (const PolyrailTerraformCommand &terraform : plan.terraforms) {
		PostPolyrailTerraformCommand(terraform);
	}
	for (const PolyrailOptimizationBridge &bridge : plan.bridges) {
		PostPolyrailOptimizationBridgeCommand(bridge);
	}
	BuildPolyrailRouteCommands(plan.rails);
}

struct PolyrailLooseEnd {
	TileIndex tile = INVALID_TILE;
	Track track = INVALID_TRACK;
	Trackdir trackdir = INVALID_TRACKDIR;
};

struct PolyrailLooseEndPair {
	PolyrailLooseEnd main;
	PolyrailLooseEnd secondary;
};

static bool IsRailEndLoose(TileIndex tile, Trackdir trackdir)
{
	DiagDirection exitdir = TrackdirToExitdir(trackdir);
	TileIndex next = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(exitdir));
	if (next == INVALID_TILE || !IsPlainRailTile(next)) return true;

	return (GetTrackBits(next) & DiagdirReachesTracks(ReverseDiagDir(exitdir))) == 0;
}

static void AddLooseRailEndsNear(TileIndex center, std::vector<PolyrailLooseEnd> &ends)
{
	int cx = static_cast<int>(TileX(center));
	int cy = static_cast<int>(TileY(center));

	for (int dy = -3; dy <= 3; dy++) {
		for (int dx = -3; dx <= 3; dx++) {
			if (std::abs(dx) + std::abs(dy) > 3) continue;
			int x = cx + dx;
			int y = cy + dy;
			if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) continue;

			TileIndex tile = TileXY(x, y);
			if (!IsPlainRailTile(tile)) continue;

			TrackBits bits = GetTrackBits(tile);
			for (Track track = TRACK_BEGIN; track != TRACK_END; track++) {
				if ((bits & TrackToTrackBits(track)) == 0) continue;
				Trackdir first = TrackToTrackdir(track);
				if (IsRailEndLoose(tile, first)) ends.push_back({tile, track, first});
				if (IsRailEndLoose(tile, ReverseTrackdir(first))) ends.push_back({tile, track, ReverseTrackdir(first)});
			}
		}
	}
}

static std::optional<PolyrailLooseEndPair> FindPolyrailLooseEndPairNear(TileIndex cursor_tile)
{
	std::vector<PolyrailLooseEnd> ends;
	AddLooseRailEndsNear(cursor_tile, ends);

	std::optional<PolyrailLooseEndPair> best;
	int best_score = INT_MAX;

	for (size_t i = 0; i < ends.size(); i++) {
		for (size_t j = i + 1; j < ends.size(); j++) {
			if (ends[i].track != ends[j].track) continue;
			if (ends[i].trackdir != ends[j].trackdir) continue;
			int ox = static_cast<int>(TileX(ends[j].tile)) - static_cast<int>(TileX(ends[i].tile));
			int oy = static_cast<int>(TileY(ends[j].tile)) - static_cast<int>(TileY(ends[i].tile));
			if (std::abs(ox) + std::abs(oy) != 1) continue;

			uint dist_i = DistanceSquare(cursor_tile, ends[i].tile);
			uint dist_j = DistanceSquare(cursor_tile, ends[j].tile);
			bool swap_pair = dist_j < dist_i;
			const PolyrailLooseEnd &primary = swap_pair ? ends[j] : ends[i];
			const PolyrailLooseEnd &secondary = swap_pair ? ends[i] : ends[j];
			int score = static_cast<int>(std::min(dist_i, dist_j) * 100 + std::max(dist_i, dist_j));
			if (score >= best_score) continue;

			best_score = score;
			best = PolyrailLooseEndPair{primary, secondary};
		}
	}

	return best;
}

static std::vector<PolyrailStart> FindPolyrailStartNear(TileIndex cursor_tile)
{
	std::vector<PolyrailStart> best(POLYRAIL_LINE_COUNT);
	best[POLYRAIL_MAIN].tile = cursor_tile;

	std::optional<PolyrailLooseEndPair> loose_ends = FindPolyrailLooseEndPairNear(cursor_tile);
	if (loose_ends.has_value()) {
		best[POLYRAIL_MAIN].tile = loose_ends->main.tile;
		best[POLYRAIL_MAIN].trackdir = loose_ends->main.trackdir;
		best[POLYRAIL_MAIN].offset = {0, 0};
		best[POLYRAIL_SECONDARY].tile = loose_ends->secondary.tile;
		best[POLYRAIL_SECONDARY].trackdir = loose_ends->secondary.trackdir;
		best[POLYRAIL_SECONDARY].offset = GetPolyrailTileOffset(loose_ends->main.tile, loose_ends->secondary.tile);
	}

	return best;
}

static std::optional<std::vector<PolyrailStart>> FindPolyrailOptimizationEndNear(TileIndex cursor_tile)
{
	std::optional<PolyrailLooseEndPair> loose_ends = FindPolyrailLooseEndPairNear(cursor_tile);
	if (!loose_ends.has_value()) return std::nullopt;

	std::vector<PolyrailStart> ends(POLYRAIL_LINE_COUNT);
	ends[POLYRAIL_MAIN].tile = AddTileIndexDiffCWrap(loose_ends->main.tile, GetPolyrailTrackdirDelta(loose_ends->main.trackdir));
	ends[POLYRAIL_MAIN].trackdir = loose_ends->main.trackdir;
	ends[POLYRAIL_MAIN].offset = {0, 0};
	ends[POLYRAIL_SECONDARY].tile = AddTileIndexDiffCWrap(loose_ends->secondary.tile, GetPolyrailTrackdirDelta(loose_ends->secondary.trackdir));
	ends[POLYRAIL_SECONDARY].trackdir = loose_ends->secondary.trackdir;
	if (ends[POLYRAIL_MAIN].tile == INVALID_TILE || ends[POLYRAIL_SECONDARY].tile == INVALID_TILE) return std::nullopt;
	ends[POLYRAIL_SECONDARY].offset = GetPolyrailTileOffset(ends[POLYRAIL_MAIN].tile, ends[POLYRAIL_SECONDARY].tile);
	return ends;
}

enum class PolyrailStartUpdateMode {
	Endpoint,
	OptimizationPreviewContinuation,
};

static bool UpdatePolyrailStartsAfterRoute(std::vector<PolyrailStart> &starts, const std::vector<PolyrailSegment> &route, PolyrailStartUpdateMode mode = PolyrailStartUpdateMode::Endpoint)
{
	if (route.empty()) return false;
	if (route.size() < starts.size()) return false;

	size_t projected_offset = route.size() - starts.size();
	for (size_t i = 0; i < starts.size(); i++) {
		const PolyrailSegment &segment = route[projected_offset + i];
		starts[i].tile = segment.end;
		starts[i].trackdir = segment.end_trackdir != INVALID_TRACKDIR ? segment.end_trackdir : TrackToTrackdir(segment.track);

		switch (mode) {
			case PolyrailStartUpdateMode::Endpoint:
				break;

			case PolyrailStartUpdateMode::OptimizationPreviewContinuation:
				starts[i].tile = GetPolyrailOptimizationContinuationTile(starts[i].tile, starts[i].trackdir);
				if (starts[i].tile == INVALID_TILE) return false;
				break;
		}
	}
	for (size_t i = 0; i < starts.size(); i++) {
		starts[i].offset = i == POLYRAIL_MAIN ? TileIndexDiffC{0, 0} : GetPolyrailTileOffset(starts[POLYRAIL_MAIN].tile, starts[i].tile);
	}

	return true;
}

static bool HandlePolyrailRoutePlacement(std::vector<PolyrailStart> &starts, const std::vector<PolyrailSegment> &route)
{
	if (route.empty()) return false;

	std::optional<PolyrailOptimizationBuildPlan> plan = BuildPolyrailRegularBuildPlanFromRoute(route);
	if (!plan.has_value()) return false;

	PostPolyrailOptimizationBuildPlan(*plan);
	return UpdatePolyrailStartsAfterRoute(starts, route);
}

static bool HandlePolyrailPointPlacement(std::vector<PolyrailStart> &starts, TileIndex end)
{
	std::vector<PolyrailSegment> route = BuildPolyrailRoute(starts, end);
	return HandlePolyrailRoutePlacement(starts, route);
}

static std::optional<PolyrailOptimizationRouteSettings> GetPolyrailOptimizationRouteSettings(std::vector<PolyrailStart> starts, TileIndex end, std::optional<Direction> forced_dir = std::nullopt)
{
	if (starts.size() != POLYRAIL_LINE_COUNT) return std::nullopt;

	std::optional<std::vector<PolyrailStart>> loose_end = forced_dir.has_value() ? std::nullopt : FindPolyrailOptimizationEndNear(end);
	Direction dir = forced_dir.value_or(INVALID_DIR);
	if (dir == INVALID_DIR) {
		dir = loose_end.has_value() ?
				GetPolyrailTrackdirDirection((*loose_end)[POLYRAIL_MAIN].trackdir) :
				ChoosePolyrailDirection(starts[POLYRAIL_MAIN].tile, end, starts[POLYRAIL_MAIN].trackdir);
	}
	if (dir == INVALID_DIR) return std::nullopt;

	std::optional<std::vector<PolyrailStart>> prepared_starts = PreparePolyrailOptimizationStarts(std::move(starts), end, dir);
	if (!prepared_starts.has_value()) return std::nullopt;

	std::vector<PolyrailStart> ends;
	if (loose_end.has_value()) {
		ends = std::move(*loose_end);
	} else {
		Trackdir end_trackdir = GetPolyrailTrackdirForDirection(dir);
		if (end_trackdir == INVALID_TRACKDIR) return std::nullopt;

		ends.resize(POLYRAIL_LINE_COUNT);
		ends[POLYRAIL_MAIN].tile = end;
		ends[POLYRAIL_MAIN].trackdir = end_trackdir;
		ends[POLYRAIL_MAIN].offset = {0, 0};
		ends[POLYRAIL_SECONDARY].tile = AddTileIndexDiffCWrap(end, (*prepared_starts)[POLYRAIL_SECONDARY].offset);
		if (ends[POLYRAIL_SECONDARY].tile == INVALID_TILE) return std::nullopt;
		ends[POLYRAIL_SECONDARY].trackdir = end_trackdir;
		ends[POLYRAIL_SECONDARY].offset = (*prepared_starts)[POLYRAIL_SECONDARY].offset;
	}

	return PolyrailOptimizationRouteSettings{std::move(*prepared_starts), std::move(ends)};
}

static bool HandlePolyrailOptimizationPlacement(std::vector<PolyrailStart> &starts, TileIndex end, std::optional<Direction> forced_dir = std::nullopt)
{
	std::optional<PolyrailOptimizationRouteSettings> settings = GetPolyrailOptimizationRouteSettings(starts, end, forced_dir);
	if (!settings.has_value()) return false;

	std::optional<std::vector<std::vector<PolyrailSegment>>> line_routes = BuildPolyrailOptimizationLineRoutes(*settings);
	if (!line_routes.has_value()) return false;

	std::optional<PolyrailOptimizationBuildPlan> plan = BuildPolyrailOptimizationBuildPlanFromLineRoutes(*line_routes);
	if (!plan.has_value()) return false;

	PostPolyrailOptimizationBuildPlan(*plan);
	return UpdatePolyrailStartsAfterRoute(starts, plan->final_route);
}

/**
 * Build new signals or remove signals or (if only one tile marked) edit a signal.
 *
 * If one tile marked abort and use GenericPlaceSignals()
 * else use CmdBuildSingleSignal() or CmdRemoveSingleSignal() in rail_cmd.cpp to build many signals
 */
static void HandleAutoSignalPlacement()
{
	Track track = (Track)GB(_thd.drawstyle, 0, 3); // 0..5

	if ((_thd.drawstyle & HT_DRAG_MASK) == HT_RECT) { // one tile case
		GenericPlaceSignals(TileVirtXY(_thd.selend.x, _thd.selend.y));
		return;
	}

	/* _settings_client.gui.drag_signals_density is given as a parameter such that each user
	 * in a network game can specify their own signal density */
	if (_remove_button_clicked) {
		Command<Commands::RemoveSignalLong>::Post(STR_ERROR_CAN_T_REMOVE_SIGNALS_FROM, CcPlaySound_CONSTRUCTION_RAIL,
				TileVirtXY(_thd.selstart.x, _thd.selstart.y), TileVirtXY(_thd.selend.x, _thd.selend.y), track, _ctrl_pressed);
	} else {
		bool sig_gui = FindWindowById(WC_BUILD_SIGNAL, 0) != nullptr;
		SignalType sigtype = sig_gui ? _cur_signal_type : _settings_client.gui.default_signal_type;
		SignalVariant sigvar = sig_gui ? _cur_signal_variant : (TimerGameCalendar::year < _settings_client.gui.semaphore_build_before ? SIG_SEMAPHORE : SIG_ELECTRIC);
		Command<Commands::BuildSignalLong>::Post(STR_ERROR_CAN_T_BUILD_SIGNALS_HERE, CcPlaySound_CONSTRUCTION_RAIL,
				TileVirtXY(_thd.selstart.x, _thd.selstart.y), TileVirtXY(_thd.selend.x, _thd.selend.y), track, sigtype, sigvar, false, _ctrl_pressed, !_settings_client.gui.drag_signals_fixed_distance, _settings_client.gui.drag_signals_density);
	}
}


/** Rail toolbar management class. */
struct BuildRailToolbarWindow : Window {
	RailType railtype = INVALID_RAILTYPE; ///< Rail type to build.
	WidgetID last_user_action = INVALID_WIDGET; ///< Last started user action.
	bool polyrail_prefix = false; ///< Space was pressed and should combine with the next autorail hotkey.

	BuildRailToolbarWindow(WindowDesc &desc, RailType railtype) : Window(desc), railtype(railtype)
	{
		this->CreateNestedTree();
		this->FinishInitNested(TRANSPORT_RAIL);
		this->DisableWidget(WID_RAT_REMOVE);
		this->OnInvalidateData();

		if (_settings_client.gui.link_terraform_toolbar) ShowTerraformToolbar(this);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		ClearPolyrailPreview();
		_polyrail_start.reset();
		if (this->IsWidgetLowered(WID_RAT_BUILD_STATION)) SetViewportCatchmentStation(nullptr, true);
		if (this->IsWidgetLowered(WID_RAT_BUILD_WAYPOINT)) SetViewportCatchmentWaypoint(nullptr, true);
		if (_settings_client.gui.link_terraform_toolbar) CloseWindowById(WC_SCEN_LAND_GEN, 0, false);
		CloseWindowById(WC_SELECT_STATION, 0);
		this->Window::Close();
	}

	/** List of widgets to be disabled if infrastructure limit prevents building. */
	static inline const std::initializer_list<WidgetID> can_build_widgets = {
		WID_RAT_BUILD_NS, WID_RAT_BUILD_X, WID_RAT_BUILD_EW, WID_RAT_BUILD_Y, WID_RAT_AUTORAIL, WID_RAT_POLYRAIL, WID_RAT_POLYRAIL_OPTIMIZATION,
		WID_RAT_BUILD_DEPOT, WID_RAT_BUILD_WAYPOINT, WID_RAT_BUILD_STATION, WID_RAT_BUILD_SIGNALS,
		WID_RAT_BUILD_BRIDGE, WID_RAT_BUILD_TUNNEL, WID_RAT_CONVERT_RAIL,
	};

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;

		if (!ValParamRailType(this->railtype)) {
			/* Close toolbar if rail type is not available. */
			this->Close();
			return;
		}

		bool can_build = CanBuildVehicleInfrastructure(VehicleType::Train);
		for (const WidgetID widget : can_build_widgets) this->SetWidgetDisabledState(widget, !can_build);
		if (!can_build) {
			CloseWindowById(WC_BUILD_SIGNAL, TRANSPORT_RAIL);
			CloseWindowById(WC_BUILD_STATION, TRANSPORT_RAIL);
			CloseWindowById(WC_BUILD_DEPOT, TRANSPORT_RAIL);
			CloseWindowById(WC_BUILD_WAYPOINT, TRANSPORT_RAIL);
			CloseWindowById(WC_SELECT_STATION, 0);
		}
	}

	bool OnTooltip([[maybe_unused]] Point pt, WidgetID widget, TooltipCloseCondition close_cond) override
	{
		bool can_build = CanBuildVehicleInfrastructure(VehicleType::Train);
		if (can_build) return false;

		if (std::ranges::find(can_build_widgets, widget) == std::end(can_build_widgets)) return false;

		GuiShowTooltips(this, GetEncodedString(STR_TOOLBAR_DISABLED_NO_VEHICLE_AVAILABLE), close_cond);
		return true;
	}

	void OnInit() override
	{
		/* Configure the rail toolbar for the railtype. */
		const RailTypeInfo *rti = GetRailTypeInfo(this->railtype);
		this->GetWidget<NWidgetCore>(WID_RAT_BUILD_NS)->SetSprite(rti->gui_sprites.build_ns_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_BUILD_X)->SetSprite(rti->gui_sprites.build_x_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_BUILD_EW)->SetSprite(rti->gui_sprites.build_ew_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_BUILD_Y)->SetSprite(rti->gui_sprites.build_y_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_AUTORAIL)->SetSprite(rti->gui_sprites.auto_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_POLYRAIL)->SetSprite(rti->gui_sprites.auto_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_POLYRAIL_OPTIMIZATION)->SetSprite(rti->gui_sprites.auto_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_BUILD_DEPOT)->SetSprite(rti->gui_sprites.build_depot);
		this->GetWidget<NWidgetCore>(WID_RAT_CONVERT_RAIL)->SetSprite(rti->gui_sprites.convert_rail);
		this->GetWidget<NWidgetCore>(WID_RAT_BUILD_TUNNEL)->SetSprite(rti->gui_sprites.build_tunnel);
	}

	static bool IsPolyrailWidget(WidgetID widget)
	{
		return widget == WID_RAT_POLYRAIL || widget == WID_RAT_POLYRAIL_OPTIMIZATION;
	}

	/**
	 * Switch to another rail type.
	 * @param railtype New rail type.
	 */
	void ModifyRailType(RailType railtype)
	{
		this->railtype = railtype;
		this->ReInit();
	}

	void UpdateRemoveWidgetStatus(WidgetID clicked_widget)
	{
		switch (clicked_widget) {
			case WID_RAT_REMOVE:
				/* If it is the removal button that has been clicked, do nothing,
				 * as it is up to the other buttons to drive removal status */
				return;

			case WID_RAT_BUILD_NS:
			case WID_RAT_BUILD_X:
			case WID_RAT_BUILD_EW:
			case WID_RAT_BUILD_Y:
			case WID_RAT_AUTORAIL:
			case WID_RAT_POLYRAIL:
			case WID_RAT_POLYRAIL_OPTIMIZATION:
			case WID_RAT_BUILD_WAYPOINT:
			case WID_RAT_BUILD_STATION:
			case WID_RAT_BUILD_SIGNALS:
				/* Removal button is enabled only if the rail/signal/waypoint/station
				 * button is still lowered.  Once raised, it has to be disabled */
				this->SetWidgetDisabledState(WID_RAT_REMOVE, !this->IsWidgetLowered(clicked_widget));
				break;

			default:
				/* When any other buttons than rail/signal/waypoint/station, raise and
				 * disable the removal button */
				this->DisableWidget(WID_RAT_REMOVE);
				this->RaiseWidget(WID_RAT_REMOVE);
				break;
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_RAT_CAPTION) {
			const RailTypeInfo *rti = GetRailTypeInfo(this->railtype);
			if (rti->max_speed > 0) {
				return GetString(STR_TOOLBAR_RAILTYPE_VELOCITY, rti->strings.toolbar_caption, PackVelocity(rti->max_speed, VehicleType::Train));
			}
			return GetString(rti->strings.toolbar_caption);
		}

		return this->Window::GetWidgetString(widget, stringid);
	}

	/**
	 * Returns corresponding cursor for provided button.
	 * @param widget Widget ID of the button.
	 * @return Corresponding cursor ID.
	 */
	CursorID GetCursorForWidget(WidgetID widget)
	{
		switch (widget) {
			case WID_RAT_BUILD_NS: return GetRailTypeInfo(_cur_railtype)->cursor.rail_ns;
			case WID_RAT_BUILD_X: return GetRailTypeInfo(_cur_railtype)->cursor.rail_swne;
			case WID_RAT_BUILD_EW: return GetRailTypeInfo(_cur_railtype)->cursor.rail_ew;
			case WID_RAT_BUILD_Y: return GetRailTypeInfo(_cur_railtype)->cursor.rail_nwse;
			case WID_RAT_AUTORAIL: return GetRailTypeInfo(_cur_railtype)->cursor.autorail;
			case WID_RAT_POLYRAIL: return GetRailTypeInfo(_cur_railtype)->cursor.autorail;
			case WID_RAT_POLYRAIL_OPTIMIZATION: return GetRailTypeInfo(_cur_railtype)->cursor.autorail;
			case WID_RAT_DEMOLISH: return ANIMCURSOR_DEMOLISH;
			case WID_RAT_BUILD_DEPOT: return GetRailTypeInfo(_cur_railtype)->cursor.depot;
			case WID_RAT_BUILD_WAYPOINT: return SPR_CURSOR_WAYPOINT;
			case WID_RAT_BUILD_STATION: return SPR_CURSOR_RAIL_STATION;
			case WID_RAT_BUILD_SIGNALS: return ANIMCURSOR_BUILDSIGNALS;
			case WID_RAT_BUILD_BRIDGE: return SPR_CURSOR_BRIDGE;
			case WID_RAT_BUILD_TUNNEL: return GetRailTypeInfo(_cur_railtype)->cursor.tunnel;
			case WID_RAT_CONVERT_RAIL: return GetRailTypeInfo(_cur_railtype)->cursor.convert;
			default: NOT_REACHED();
		}
	}

	/**
	 * Returns corresponding high light style for provided button.
	 * @param widget Widget ID of the button.
	 * @return Corresponding high light style.
	 */
	HighLightStyle GetHighLightStyleForWidget(WidgetID widget)
	{
		switch (widget) {
			case WID_RAT_BUILD_NS: return HT_LINE | HT_DIR_VL;
			case WID_RAT_BUILD_X: return HT_LINE | HT_DIR_X;
			case WID_RAT_BUILD_EW: return HT_LINE | HT_DIR_HL;
			case WID_RAT_BUILD_Y: return HT_LINE | HT_DIR_Y;
			case WID_RAT_AUTORAIL: return HT_RAIL;
			case WID_RAT_POLYRAIL: return HT_RAIL;
			case WID_RAT_POLYRAIL_OPTIMIZATION: return HT_RAIL;
			case WID_RAT_DEMOLISH: return HT_RECT | HT_DIAGONAL;
			case WID_RAT_BUILD_DEPOT: return HT_RECT;
			case WID_RAT_BUILD_WAYPOINT: return HT_RECT;
			case WID_RAT_BUILD_STATION: return HT_RECT;
			case WID_RAT_BUILD_SIGNALS: return HT_RECT;
			case WID_RAT_BUILD_BRIDGE: return HT_RECT;
			case WID_RAT_BUILD_TUNNEL: return HT_SPECIAL;
			case WID_RAT_CONVERT_RAIL: return HT_RECT | HT_DIAGONAL;
			default: NOT_REACHED();
		}
	}


	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget < WID_RAT_BUILD_NS) return;

		_remove_button_clicked = false;

		if (widget == WID_RAT_REMOVE) {
			BuildRailClick_Remove(this);
			if (_ctrl_pressed) RailToolbar_CtrlChanged(this);
			return;
		}

		if (!IsPolyrailWidget(widget)) {
			ClearPolyrailPreview();
			_polyrail_start.reset();
		}

		this->last_user_action = widget;
		bool started = HandlePlacePushButton(this, widget, this->GetCursorForWidget(widget), this->GetHighLightStyleForWidget(widget));
		if (started) {
			_thd.select_proc = IsPolyrailWidget(widget) ? DDSP_PLACE_POLYRAIL : DDSP_PLACE_RAIL;
		} else if (IsPolyrailWidget(widget)) {
			ClearPolyrailPreview();
			if (!this->IsWidgetLowered(WID_RAT_POLYRAIL) && !this->IsWidgetLowered(WID_RAT_POLYRAIL_OPTIMIZATION)) _polyrail_start.reset();
		}

		switch (widget) {
			case WID_RAT_BUILD_DEPOT:
				if (started) {
					ShowBuildTrainDepotPicker(this);
				}
				break;

			case WID_RAT_BUILD_WAYPOINT:
				if (started) {
					ShowBuildWaypointPicker(this);
				}
				break;

			case WID_RAT_BUILD_STATION:
				if (started) {
					ShowStationBuilder(this);
				}
				break;

			case WID_RAT_BUILD_SIGNALS: {
				if (started != _ctrl_pressed) {
					ShowSignalBuilder(this);
				}
				break;
			}
		}

		this->UpdateRemoveWidgetStatus(widget);
		if (_ctrl_pressed) RailToolbar_CtrlChanged(this);
	}

	bool IsPolyrailActive() const
	{
		return (this->IsWidgetLowered(WID_RAT_POLYRAIL) || this->IsWidgetLowered(WID_RAT_POLYRAIL_OPTIMIZATION)) && _thd.select_proc == DDSP_PLACE_POLYRAIL;
	}

	std::vector<PolyrailSegment> BuildActivePolyrailRoute(const std::vector<PolyrailStart> &starts, TileIndex cursor) const
	{
		if (!this->IsWidgetLowered(WID_RAT_POLYRAIL_OPTIMIZATION)) return BuildPolyrailRoute(starts, cursor);

		std::optional<PolyrailOptimizationRouteSettings> settings = GetPolyrailOptimizationRouteSettings(starts, cursor);
		return settings.has_value() ? BuildPolyrailOptimizationRoute(*settings) : std::vector<PolyrailSegment>{};
	}

	std::vector<PolyrailSegment> BuildActivePolyrailPreviewRoute(const std::vector<PolyrailStart> &starts, TileIndex cursor) const
	{
		if (!this->IsWidgetLowered(WID_RAT_POLYRAIL_OPTIMIZATION)) return this->BuildActivePolyrailRoute(starts, cursor);

		std::optional<PolyrailOptimizationRouteSettings> settings = GetPolyrailOptimizationRouteSettings(starts, cursor);
		if (!settings.has_value()) return {};

		std::optional<PolyrailOptimizationBuildPlan> plan = BuildPolyrailOptimizationBuildPlan(*settings);
		return plan.has_value() ? plan->final_route : std::vector<PolyrailSegment>{};
	}

	std::optional<PolyrailOptimizationBuildPlan> BuildActivePolyrailBuildPlanFromRoute(const std::vector<PolyrailSegment> &route) const
	{
		return this->IsWidgetLowered(WID_RAT_POLYRAIL_OPTIMIZATION) ?
				BuildPolyrailOptimizationBuildPlanFromInterleavedRoute(route) :
				BuildPolyrailRegularBuildPlanFromRoute(route);
	}

	std::vector<PolyrailSegment> BuildValidatedPolyrailPreviewRoute(const std::vector<PolyrailStart> &starts, TileIndex cursor) const
	{
		std::vector<PolyrailSegment> route = this->BuildActivePolyrailPreviewRoute(starts, cursor);
		if (route.empty()) return {};

		return this->BuildActivePolyrailBuildPlanFromRoute(route).has_value() ? route : std::vector<PolyrailSegment>{};
	}

	std::vector<PolyrailSegment> BuildValidatedPolyrailPreviewRouteWithDirection(const std::vector<PolyrailStart> &starts, TileIndex cursor, Direction dir, bool force_connected_starts) const
	{
		std::vector<PolyrailSegment> route = BuildPolyrailRouteWithDirection(starts, cursor, dir, force_connected_starts);
		if (route.empty()) return {};

		return BuildPolyrailRegularBuildPlanFromRoute(route).has_value() ? route : std::vector<PolyrailSegment>{};
	}

	PolyrailStartUpdateMode GetPolyrailPreviewStartUpdateMode() const
	{
		return this->IsWidgetLowered(WID_RAT_POLYRAIL_OPTIMIZATION) ?
				PolyrailStartUpdateMode::OptimizationPreviewContinuation :
				PolyrailStartUpdateMode::Endpoint;
	}

	void GeneratePolyrailPreview()
	{
		if (!this->IsPolyrailActive() || !_polyrail_start.has_value()) return;

		TileIndex cursor = TileVirtXY(_thd.pos.x, _thd.pos.y);
		std::vector<PolyrailSegment> route = this->BuildValidatedPolyrailPreviewRoute(*_polyrail_start, cursor);
		if (route.empty()) {
			ClearPolyrailPreview();
			return;
		}

		SetPolyrailPreview(route);
	}

	void AppendPolyrailPreview()
	{
		if (!this->IsPolyrailActive() || !_polyrail_start.has_value()) return;
		if (!_polyrail_preview.active_route.has_value()) {
			this->GeneratePolyrailPreview();
			return;
		}

		std::vector<PolyrailStart> next_starts = _polyrail_preview.build_starts.value_or(*_polyrail_start);
		TileIndex cursor = TileVirtXY(_thd.pos.x, _thd.pos.y);
		const std::vector<PolyrailSegment> &active_route = *_polyrail_preview.active_route;
		if (active_route.size() < next_starts.size()) return;
		size_t projected_offset = active_route.size() - next_starts.size();
		TileIndex last_primary_end = active_route[projected_offset + POLYRAIL_MAIN].end;

		std::vector<PolyrailSegment> route;
		if (this->IsWidgetLowered(WID_RAT_POLYRAIL_OPTIMIZATION)) {
			if (!UpdatePolyrailStartsAfterRoute(next_starts, active_route, PolyrailStartUpdateMode::OptimizationPreviewContinuation)) return;
			route = this->BuildValidatedPolyrailPreviewRoute(next_starts, cursor);
		} else {
			Direction dir = ChoosePolyrailDirection(last_primary_end, cursor, INVALID_TRACKDIR);
			if (!UpdatePolyrailStartsAfterRoute(next_starts, active_route)) return;
			route = this->BuildValidatedPolyrailPreviewRouteWithDirection(next_starts, cursor, dir, true);
		}
		if (route.empty()) return;

		_polyrail_preview.stored_routes.push_back(std::move(*_polyrail_preview.active_route));
		_polyrail_preview.build_starts = std::move(next_starts);
		_polyrail_preview.active_route = std::move(route);
		LogPolyrailPreviewRoute(*_polyrail_preview.active_route);
		UpdatePolyrailPreviewHighlight();
	}

	void RemoveNewestPolyrailPreview()
	{
		if (!this->IsPolyrailActive() || !_polyrail_start.has_value()) return;
		if (_polyrail_preview.stored_routes.empty() && !_polyrail_preview.active_route.has_value()) return;

		if (_polyrail_preview.active_route.has_value()) {
			_polyrail_preview.active_route.reset();
			if (!_polyrail_preview.stored_routes.empty()) {
				_polyrail_preview.active_route = std::move(_polyrail_preview.stored_routes.back());
				_polyrail_preview.stored_routes.pop_back();
			}
		} else {
			_polyrail_preview.stored_routes.pop_back();
		}

		if (_polyrail_preview.stored_routes.empty() && !_polyrail_preview.active_route.has_value()) {
			ClearPolyrailPreview();
			return;
		}

		std::vector<PolyrailStart> starts = *_polyrail_start;
		for (const std::vector<PolyrailSegment> &route : _polyrail_preview.stored_routes) {
			if (!UpdatePolyrailStartsAfterRoute(starts, route, this->GetPolyrailPreviewStartUpdateMode())) {
				ClearPolyrailPreview();
				return;
			}
		}
		_polyrail_preview.build_starts = _polyrail_preview.stored_routes.empty() ? std::nullopt : std::optional<std::vector<PolyrailStart>>{std::move(starts)};
		UpdatePolyrailPreviewHighlight();
	}

	void BuildPolyrailPreview()
	{
		if (!this->IsPolyrailActive() || !_polyrail_start.has_value()) return;
		if (_polyrail_preview.stored_routes.empty() && !_polyrail_preview.active_route.has_value()) return;

		std::vector<const std::vector<PolyrailSegment> *> routes;
		routes.reserve(_polyrail_preview.stored_routes.size() + (_polyrail_preview.active_route.has_value() ? 1 : 0));
		for (const std::vector<PolyrailSegment> &route : _polyrail_preview.stored_routes) routes.push_back(&route);
		if (_polyrail_preview.active_route.has_value()) routes.push_back(&*_polyrail_preview.active_route);

		std::vector<PolyrailOptimizationBuildPlan> plans;
		plans.reserve(routes.size());
		for (const std::vector<PolyrailSegment> *route : routes) {
			std::optional<PolyrailOptimizationBuildPlan> plan = this->BuildActivePolyrailBuildPlanFromRoute(*route);
			if (!plan.has_value()) return;
			plans.push_back(std::move(*plan));
		}

		for (const PolyrailOptimizationBuildPlan &plan : plans) PostPolyrailOptimizationBuildPlan(plan);
		if (UpdatePolyrailStartsAfterRoute(*_polyrail_start, *routes.back())) ClearPolyrailPreview();
	}

	EventState OnHotkey(int hotkey) override
	{
		if (hotkey == RTHK_POLYRAIL_PREFIX) {
			this->polyrail_prefix = true;
			return ES_HANDLED;
		}

		if (this->polyrail_prefix) {
			this->polyrail_prefix = false;
			if (hotkey == RTHK_POLYRAIL_PREVIEW) {
				if (!this->IsPolyrailActive()) return ES_NOT_HANDLED;
				this->AppendPolyrailPreview();
				return ES_HANDLED;
			}
			if (hotkey == WID_RAT_AUTORAIL) {
				MarkTileDirtyByTile(TileVirtXY(_thd.pos.x, _thd.pos.y)); // redraw tile selection
				this->OnClick(Point(), WID_RAT_POLYRAIL, 1);
				return ES_HANDLED;
			}
			if (hotkey == WID_RAT_DEMOLISH) {
				MarkTileDirtyByTile(TileVirtXY(_thd.pos.x, _thd.pos.y)); // redraw tile selection
				this->OnClick(Point(), WID_RAT_POLYRAIL_OPTIMIZATION, 1);
				return ES_HANDLED;
			}
		}

		if (hotkey == RTHK_POLYRAIL_PREVIEW) {
			if (!this->IsPolyrailActive()) return ES_NOT_HANDLED;
			this->GeneratePolyrailPreview();
			return ES_HANDLED;
		}

		if (hotkey == RTHK_POLYRAIL_BUILD_PREVIEW) {
			if (!this->IsPolyrailActive()) return ES_NOT_HANDLED;
			this->BuildPolyrailPreview();
			return ES_HANDLED;
		}

		if (hotkey == RTHK_POLYRAIL_REMOVE_PREVIEW) {
			if (!this->IsPolyrailActive()) return ES_NOT_HANDLED;
			this->RemoveNewestPolyrailPreview();
			return ES_HANDLED;
		}

		if (IsSpecialHotkey(hotkey)) return this->ChangeRailTypeOnHotkey(hotkey);
		MarkTileDirtyByTile(TileVirtXY(_thd.pos.x, _thd.pos.y)); // redraw tile selection
		return Window::OnHotkey(hotkey);
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_RAT_BUILD_NS:
				VpStartPlaceSizing(tile, VPM_FIX_VERTICAL | VPM_RAILDIRS, DDSP_PLACE_RAIL);
				break;

			case WID_RAT_BUILD_X:
				VpStartPlaceSizing(tile, VPM_FIX_Y | VPM_RAILDIRS, DDSP_PLACE_RAIL);
				break;

			case WID_RAT_BUILD_EW:
				VpStartPlaceSizing(tile, VPM_FIX_HORIZONTAL | VPM_RAILDIRS, DDSP_PLACE_RAIL);
				break;

			case WID_RAT_BUILD_Y:
				VpStartPlaceSizing(tile, VPM_FIX_X | VPM_RAILDIRS, DDSP_PLACE_RAIL);
				break;

			case WID_RAT_AUTORAIL:
				VpStartPlaceSizing(tile, VPM_RAILDIRS, DDSP_PLACE_RAIL);
				break;

			case WID_RAT_POLYRAIL:
			case WID_RAT_POLYRAIL_OPTIMIZATION:
				if (!_polyrail_start.has_value()) {
					ClearPolyrailPreview();
					_polyrail_start = FindPolyrailStartNear(tile);
					SndClickBeep();
				} else {
					ClearPolyrailPreview();
					if (this->last_user_action == WID_RAT_POLYRAIL_OPTIMIZATION) {
						VpStartPlaceSizing(tile, VPM_RAILDIRS, DDSP_PLACE_POLYRAIL);
					} else {
						HandlePolyrailPointPlacement(*_polyrail_start, tile);
					}
				}
				break;

			case WID_RAT_DEMOLISH:
				PlaceProc_DemolishArea(tile);
				break;

			case WID_RAT_BUILD_DEPOT:
				Command<Commands::BuildRailDepot>::Post(STR_ERROR_CAN_T_BUILD_TRAIN_DEPOT, CcRailDepot, tile, _cur_railtype, _build_depot_direction);
				break;

			case WID_RAT_BUILD_WAYPOINT:
				PlaceRail_Waypoint(tile);
				break;

			case WID_RAT_BUILD_STATION:
				PlaceRail_Station(tile);
				break;

			case WID_RAT_BUILD_SIGNALS:
				VpStartPlaceSizing(tile, VPM_SIGNALDIRS, DDSP_BUILD_SIGNALS);
				break;

			case WID_RAT_BUILD_BRIDGE:
				PlaceRail_Bridge(tile, this);
				break;

			case WID_RAT_BUILD_TUNNEL:
				Command<Commands::BuildTunnel>::Post(STR_ERROR_CAN_T_BUILD_TUNNEL_HERE, CcBuildRailTunnel, tile, TRANSPORT_RAIL, _cur_railtype, INVALID_ROADTYPE);
				break;

			case WID_RAT_CONVERT_RAIL:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_CONVERT_RAIL);
				break;

			default: NOT_REACHED();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		/* no dragging if you have pressed the convert button */
		if (FindWindowById(WC_BUILD_SIGNAL, 0) != nullptr && _convert_signal_button && this->IsWidgetLowered(WID_RAT_BUILD_SIGNALS)) return;

		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return AlignInitialConstructionToolbar(sm_width);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (pt.x != -1) {
			switch (select_proc) {
				default: NOT_REACHED();
				case DDSP_BUILD_BRIDGE:
					if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
					ShowBuildBridgeWindow(start_tile, end_tile, TRANSPORT_RAIL, _cur_railtype, INVALID_ROADTYPE);
					break;

				case DDSP_PLACE_RAIL:
					ClearPolyrailPreview();
					HandleAutodirPlacement();
					break;

				case DDSP_PLACE_POLYRAIL:
					ClearPolyrailPreview();
					if (this->last_user_action == WID_RAT_POLYRAIL_OPTIMIZATION) {
						if (!_polyrail_start.has_value()) break;

						if (start_tile != end_tile) {
							Direction dir = ChoosePolyrailDirectionFromHighlight(start_tile, end_tile, _thd.drawstyle);
							if (dir == INVALID_DIR) dir = ChoosePolyrailDirection(start_tile, end_tile, INVALID_TRACKDIR);
							HandlePolyrailOptimizationPlacement(*_polyrail_start, start_tile, dir);
						} else {
							HandlePolyrailOptimizationPlacement(*_polyrail_start, start_tile);
						}
					} else {
						HandlePolyrailPlacement();
					}
					break;

				case DDSP_BUILD_SIGNALS:
					HandleAutoSignalPlacement();
					break;

				case DDSP_DEMOLISH_AREA:
					GUIPlaceProcDragXY(select_proc, start_tile, end_tile);
					break;

				case DDSP_CONVERT_RAIL:
					Command<Commands::ConvertRail>::Post(STR_ERROR_CAN_T_CONVERT_RAIL, CcPlaySound_CONSTRUCTION_RAIL, end_tile, start_tile, _cur_railtype, _ctrl_pressed);
					break;

				case DDSP_REMOVE_STATION:
				case DDSP_BUILD_STATION:
					if (this->IsWidgetLowered(WID_RAT_BUILD_STATION)) {
						/* Station */
						if (_remove_button_clicked) {
							bool keep_rail = !_ctrl_pressed;
							Command<Commands::RemoveFromRailStation>::Post(STR_ERROR_CAN_T_REMOVE_PART_OF_STATION, CcPlaySound_CONSTRUCTION_RAIL, end_tile, start_tile, keep_rail);
						} else {
							HandleStationPlacement(start_tile, end_tile);
						}
					} else {
						/* Waypoint */
						if (_remove_button_clicked) {
							bool keep_rail = !_ctrl_pressed;
							Command<Commands::RemoveFromRailWaypoint>::Post(STR_ERROR_CAN_T_REMOVE_RAIL_WAYPOINT , CcPlaySound_CONSTRUCTION_RAIL, end_tile, start_tile, keep_rail);
						} else {
							TileArea ta(start_tile, end_tile);
							Axis axis = select_method == VPM_X_LIMITED ? AXIS_X : AXIS_Y;
							bool adjacent = _ctrl_pressed;

							auto proc = [=](bool test, StationID to_join) -> bool {
								if (test) {
									return Command<Commands::BuildRailWaypoint>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildRailWaypoint>()), ta.tile, axis, ta.w, ta.h, _waypoint_gui.sel_class, _waypoint_gui.sel_type, StationID::Invalid(), adjacent).Succeeded();
								} else {
									return Command<Commands::BuildRailWaypoint>::Post(STR_ERROR_CAN_T_BUILD_RAIL_WAYPOINT , CcPlaySound_CONSTRUCTION_RAIL, ta.tile, axis, ta.w, ta.h, _waypoint_gui.sel_class, _waypoint_gui.sel_type, to_join, adjacent);
								}
							};

							ShowSelectRailWaypointIfNeeded(ta, proc);
						}
					}
					break;
			}
		}
	}

	void OnPlaceObjectAbort() override
	{
		ClearPolyrailPreview();
		_polyrail_start.reset();
		if (this->IsWidgetLowered(WID_RAT_BUILD_STATION)) SetViewportCatchmentStation(nullptr, true);
		if (this->IsWidgetLowered(WID_RAT_BUILD_WAYPOINT)) SetViewportCatchmentWaypoint(nullptr, true);

		this->RaiseButtons();
		this->DisableWidget(WID_RAT_REMOVE);
		this->SetWidgetDirty(WID_RAT_REMOVE);

		CloseWindowById(WC_BUILD_SIGNAL, TRANSPORT_RAIL);
		CloseWindowById(WC_BUILD_STATION, TRANSPORT_RAIL);
		CloseWindowById(WC_BUILD_DEPOT, TRANSPORT_RAIL);
		CloseWindowById(WC_BUILD_WAYPOINT, TRANSPORT_RAIL);
		CloseWindowById(WC_SELECT_STATION, 0);
		CloseWindowByClass(WC_BUILD_BRIDGE);
	}

	void OnPlacePresize([[maybe_unused]] Point pt, TileIndex tile) override
	{
		Command<Commands::BuildTunnel>::Do(DoCommandFlag::Auto, tile, TRANSPORT_RAIL, _cur_railtype, INVALID_ROADTYPE);
		VpSetPresizeRange(tile, _build_tunnel_endtile == 0 ? tile : _build_tunnel_endtile);
	}

	EventState OnCTRLStateChange() override
	{
		/* do not toggle Remove button by Ctrl when placing station */
		if (!this->IsWidgetLowered(WID_RAT_BUILD_STATION) && !this->IsWidgetLowered(WID_RAT_BUILD_WAYPOINT) && RailToolbar_CtrlChanged(this)) return ES_HANDLED;
		return ES_NOT_HANDLED;
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		if (this->IsWidgetLowered(WID_RAT_BUILD_WAYPOINT)) CheckRedrawRailWaypointCoverage(this);
	}

	/**
	 * Selects new RailType based on SpecialHotkeys and order defined in _sorted_railtypes.
	 * @param hotkey Defines what action to perform.
	 * @return ES_HANDLED if hotkey was accepted.
	 */
	EventState ChangeRailTypeOnHotkey(int hotkey)
	{
		auto [index, step] = GetListIndexStep(SpecialListHotkeys(hotkey), _sorted_railtypes, this->railtype);

		while (!HasRailTypeAvail(_local_company, _sorted_railtypes[index])) {
			index = (index + step) % _sorted_railtypes.size();
		}

		_last_built_railtype = _cur_railtype = _sorted_railtypes[index];
		this->ModifyRailType(_last_built_railtype);

		/* Update cursor and all sub windows. */
		if (_thd.GetCallbackWnd() == this) SetCursor(this->GetCursorForWidget(this->last_user_action), PAL_NONE);
		for (WindowClass cls : {WC_BUILD_STATION, WC_BUILD_SIGNAL, WC_BUILD_WAYPOINT, WC_BUILD_DEPOT}) SetWindowDirty(cls, TRANSPORT_RAIL);

		return ES_HANDLED;
	}

	/**
	 * Handler for global hotkeys of the BuildRailToolbarWindow.
	 * @param hotkey Hotkey
	 * @return ES_HANDLED if hotkey was accepted.
	 */
	static EventState RailToolbarGlobalHotkeys(int hotkey)
	{
		if (_game_mode != GM_NORMAL) return ES_NOT_HANDLED;
		Window *w = ShowBuildRailToolbar(_last_built_railtype);
		if (w == nullptr) return ES_NOT_HANDLED;
		return w->OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"railtoolbar", {
		Hotkey('1', "build_ns", WID_RAT_BUILD_NS),
		Hotkey('2', "build_x", WID_RAT_BUILD_X),
		Hotkey('3', "build_ew", WID_RAT_BUILD_EW),
		Hotkey('4', "build_y", WID_RAT_BUILD_Y),
		Hotkey({'5', 'A' | WKC_GLOBAL_HOTKEY}, "autorail", WID_RAT_AUTORAIL),
		Hotkey('0', "polyrail", WID_RAT_POLYRAIL),
		Hotkey(WKC_SPACE, "polyrail_prefix", RTHK_POLYRAIL_PREFIX),
		Hotkey('N', "polyrail_preview", RTHK_POLYRAIL_PREVIEW),
		Hotkey('K', "polyrail_build_preview", RTHK_POLYRAIL_BUILD_PREVIEW),
		Hotkey('I', "polyrail_remove_preview", RTHK_POLYRAIL_REMOVE_PREVIEW),
		Hotkey('6', "demolish", WID_RAT_DEMOLISH),
		Hotkey('7', "depot", WID_RAT_BUILD_DEPOT),
		Hotkey('8', "waypoint", WID_RAT_BUILD_WAYPOINT),
		Hotkey('9', "station", WID_RAT_BUILD_STATION),
		Hotkey('S', "signal", WID_RAT_BUILD_SIGNALS),
		Hotkey('B', "bridge", WID_RAT_BUILD_BRIDGE),
		Hotkey('T', "tunnel", WID_RAT_BUILD_TUNNEL),
		Hotkey('R', "remove", WID_RAT_REMOVE),
		Hotkey('C', "convert", WID_RAT_CONVERT_RAIL),
		Hotkey(WKC_L_BRACKET, "prev_railtype", to_underlying(SpecialListHotkeys::PreviousItem)),
		Hotkey(WKC_R_BRACKET, "next_railtype", to_underlying(SpecialListHotkeys::NextItem)),
		Hotkey(WKC_L_BRACKET | WKC_CTRL, "first_railtype", to_underlying(SpecialListHotkeys::FirstItem)),
		Hotkey(WKC_R_BRACKET | WKC_CTRL, "last_railtype", to_underlying(SpecialListHotkeys::LastItem)),
	}, RailToolbarGlobalHotkeys};
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_rail_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_RAT_CAPTION), SetTextStyle(TC_WHITE),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_NS),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_NS, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_TRACK),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_X),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_NE, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_TRACK),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_EW),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_EW, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_TRACK),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_Y),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_NW, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_TRACK),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_AUTORAIL),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_AUTORAIL, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_AUTORAIL),
		NWidget(WWT_IMGBTN, Colours::Orange, WID_RAT_POLYRAIL),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_AUTORAIL, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_POLYRAIL),
		NWidget(WWT_IMGBTN, Colours::Blue, WID_RAT_POLYRAIL_OPTIMIZATION),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_AUTORAIL, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_POLYRAIL_OPTIMIZATION),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_DEMOLISH),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_DYNAMITE, STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_DEPOT),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_DEPOT_RAIL, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_TRAIN_DEPOT_FOR_BUILDING),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_WAYPOINT),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_WAYPOINT, STR_RAIL_TOOLBAR_TOOLTIP_CONVERT_RAIL_TO_WAYPOINT),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_STATION),
						SetFill(0, 1), SetToolbarMinimalSize(2), SetSpriteTip(SPR_IMG_RAIL_STATION, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_STATION),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_SIGNALS),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_SIGNALS, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_SIGNALS),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_BRIDGE),
						SetFill(0, 1), SetToolbarMinimalSize(2), SetSpriteTip(SPR_IMG_BRIDGE, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_BRIDGE),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_BUILD_TUNNEL),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_TUNNEL_RAIL, STR_RAIL_TOOLBAR_TOOLTIP_BUILD_RAILROAD_TUNNEL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_REMOVE),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_REMOVE, STR_RAIL_TOOLBAR_TOOLTIP_TOGGLE_BUILD_REMOVE_FOR),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RAT_CONVERT_RAIL),
						SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_CONVERT_RAIL, STR_RAIL_TOOLBAR_TOOLTIP_CONVERT_RAIL),
	EndContainer(),
};

/** Window definition for the rail toolbar. */
static WindowDesc _build_rail_desc(
	WindowPosition::Manual, "toolbar_rail", 0, 0,
	WC_BUILD_TOOLBAR, WC_NONE,
	WindowDefaultFlag::Construction,
	_nested_build_rail_widgets,
	&BuildRailToolbarWindow::hotkeys
);


/**
 * Open the build rail toolbar window for a specific rail type.
 *
 * If the terraform toolbar is linked to the toolbar, that window is also opened.
 *
 * @param railtype Rail type to open the window for
 * @return newly opened rail toolbar, or nullptr if the toolbar could not be opened.
 */
Window *ShowBuildRailToolbar(RailType railtype)
{
	if (!Company::IsValidID(_local_company)) return nullptr;
	if (!ValParamRailType(railtype)) return nullptr;

	CloseWindowByClass(WC_BUILD_TOOLBAR);
	_cur_railtype = railtype;
	_remove_button_clicked = false;
	return new BuildRailToolbarWindow(_build_rail_desc, railtype);
}

/* TODO: For custom stations, respect their allowed platforms/lengths bitmasks!
 * --pasky */

static void HandleStationPlacement(TileIndex start, TileIndex end)
{
	TileArea ta(start, end);
	uint numtracks = ta.w;
	uint platlength = ta.h;

	if (_station_gui.axis == AXIS_X) std::swap(numtracks, platlength);

	StationPickerSelection params = _station_gui;
	RailType rt = _cur_railtype;
	bool adjacent = _ctrl_pressed;

	auto proc = [=](bool test, StationID to_join) -> bool {
		if (test) {
			return Command<Commands::BuildRailStation>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildRailStation>()), ta.tile, rt, params.axis, numtracks, platlength, params.sel_class, params.sel_type, StationID::Invalid(), adjacent).Succeeded();
		} else {
			return Command<Commands::BuildRailStation>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_STATION, CcStation, ta.tile, rt, params.axis, numtracks, platlength, params.sel_class, params.sel_type, to_join, adjacent);
		}
	};

	ShowSelectStationIfNeeded(ta, proc);
}

/**
 * Test if a station/waypoint uses the default graphics.
 * @param bst Station to test.
 * @return true if at least one of its rail station tiles uses the default graphics.
 */
static bool StationUsesDefaultType(const BaseStation *bst)
{
	for (TileIndex t : bst->train_station) {
		if (bst->TileBelongsToRailStation(t) && HasStationRail(t) && GetCustomStationSpecIndex(t) == 0) return true;
	}
	return false;
}

class StationPickerCallbacks : public PickerCallbacksNewGRFClass<StationClass> {
public:
	StationPickerCallbacks() : PickerCallbacksNewGRFClass<StationClass>("fav_stations") {}

	GrfSpecFeature GetFeature() const override { return GrfSpecFeature::Stations; }

	StringID GetClassTooltip() const override { return STR_PICKER_STATION_CLASS_TOOLTIP; }
	StringID GetTypeTooltip() const override { return STR_PICKER_STATION_TYPE_TOOLTIP; }
	StringID GetCollectionTooltip() const override { return STR_PICKER_STATION_COLLECTION_TOOLTIP; }

	bool IsActive() const override
	{
		for (const auto &cls : StationClass::Classes()) {
			if (IsWaypointClass(cls)) continue;
			for (const auto *spec : cls.Specs()) {
				if (spec != nullptr) return true;
			}
		}
		return false;
	}

	bool HasClassChoice() const override
	{
		return std::ranges::count_if(StationClass::Classes(), [](const auto &cls) { return !IsWaypointClass(cls); }) > 1;
	}

	int GetSelectedClass() const override { return _station_gui.sel_class.base(); }
	void SetSelectedClass(int id) const override { _station_gui.sel_class = this->GetClassIndex(id); }

	StringID GetClassName(int id) const override
	{
		const auto *sc = GetClass(id);
		if (IsWaypointClass(*sc)) return INVALID_STRING_ID;
		return sc->name;
	}

	int GetSelectedType() const override { return _station_gui.sel_type; }
	void SetSelectedType(int id) const override { _station_gui.sel_type = id; }

	StringID GetTypeName(int cls_id, int id) const override
	{
		const auto *spec = this->GetSpec(cls_id, id);
		return (spec == nullptr) ? STR_STATION_CLASS_DFLT_STATION : spec->name;
	}

	std::span<const BadgeID> GetTypeBadges(int cls_id, int id) const override
	{
		const auto *spec = this->GetSpec(cls_id, id);
		if (spec == nullptr) return {};
		return spec->badges;
	}

	bool IsTypeAvailable(int cls_id, int id) const override
	{
		return IsStationAvailable(this->GetSpec(cls_id, id));
	}

	void DrawType(int x, int y, int cls_id, int id) const override
	{
		if (!DrawStationTile(x, y, _cur_railtype, _station_gui.axis, this->GetClassIndex(cls_id), id)) {
			StationPickerDrawSprite(x, y, StationType::Rail, _cur_railtype, INVALID_ROADTYPE, 2 + _station_gui.axis);
		}
	}

	void FillUsedItems(std::set<PickerItem> &items) override
	{
		bool default_added = false;
		for (const Station *st : Station::Iterate()) {
			if (st->owner != _local_company) continue;
			if (!default_added && StationUsesDefaultType(st)) {
				items.insert({0, 0, STAT_CLASS_DFLT.base(), 0});
				default_added = true;
			}
			for (const auto &sm : st->speclist) {
				if (sm.spec == nullptr) continue;
				items.insert({sm.grfid, sm.localidx, sm.spec->class_index.base(), sm.spec->index});
			}
		}
	}

	static StationPickerCallbacks instance;
};
/* static */ StationPickerCallbacks StationPickerCallbacks::instance;

struct BuildRailStationWindow : public PickerWindow {
private:
	uint coverage_height = 0; ///< Height of the coverage texts.

	/**
	 * Verify whether the currently selected station size is allowed after selecting a new station class/type.
	 * If not, change the station size variables ( _settings_client.gui.station_numtracks and _settings_client.gui.station_platlength ).
	 * @param statspec Specification of the new station class/type
	 */
	void CheckSelectedSize(const StationSpec *statspec)
	{
		if (statspec == nullptr || _settings_client.gui.station_dragdrop) return;

		/* If current number of tracks is not allowed, make it as big as possible */
		if (HasBit(statspec->disallowed_platforms, _settings_client.gui.station_numtracks - 1)) {
			this->RaiseWidget(_settings_client.gui.station_numtracks + WID_BRAS_PLATFORM_NUM_BEGIN);
			_settings_client.gui.station_numtracks = 1;
			if (statspec->disallowed_platforms != UINT8_MAX) {
				while (HasBit(statspec->disallowed_platforms, _settings_client.gui.station_numtracks - 1)) {
					_settings_client.gui.station_numtracks++;
				}
				this->LowerWidget(_settings_client.gui.station_numtracks + WID_BRAS_PLATFORM_NUM_BEGIN);
			}
		}

		if (HasBit(statspec->disallowed_lengths, _settings_client.gui.station_platlength - 1)) {
			this->RaiseWidget(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN);
			_settings_client.gui.station_platlength = 1;
			if (statspec->disallowed_lengths != UINT8_MAX) {
				while (HasBit(statspec->disallowed_lengths, _settings_client.gui.station_platlength - 1)) {
					_settings_client.gui.station_platlength++;
				}
				this->LowerWidget(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN);
			}
		}
	}

public:
	BuildRailStationWindow(WindowDesc &desc, Window *parent) : PickerWindow(desc, parent, TRANSPORT_RAIL, StationPickerCallbacks::instance)
	{
		this->coverage_height = 2 * GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
		this->ConstructWindow();
	}

	void OnInit() override
	{
		this->LowerWidget(WID_BRAS_PLATFORM_DIR_X + _station_gui.axis);
		if (_settings_client.gui.station_dragdrop) {
			this->LowerWidget(WID_BRAS_PLATFORM_DRAG_N_DROP);
		} else {
			this->LowerWidget(WID_BRAS_PLATFORM_NUM_BEGIN + _settings_client.gui.station_numtracks);
			this->LowerWidget(WID_BRAS_PLATFORM_LEN_BEGIN + _settings_client.gui.station_platlength);
		}
		this->SetWidgetLoweredState(WID_BRAS_HIGHLIGHT_OFF, !_settings_client.gui.station_show_coverage);
		this->SetWidgetLoweredState(WID_BRAS_HIGHLIGHT_ON, _settings_client.gui.station_show_coverage);

		this->PickerWindow::OnInit();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		CloseWindowById(WC_SELECT_STATION, 0);
		this->PickerWindow::Close();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (gui_scope) {
			const StationSpec *statspec = StationClass::Get(_station_gui.sel_class)->GetSpec(_station_gui.sel_type);
			this->CheckSelectedSize(statspec);
		}

		this->PickerWindow::OnInvalidateData(data, gui_scope);
	}

	void OnPaint() override
	{
		const StationSpec *statspec = StationClass::Get(_station_gui.sel_class)->GetSpec(_station_gui.sel_type);

		if (_settings_client.gui.station_dragdrop) {
			SetTileSelectSize(1, 1);
		} else {
			int x = _settings_client.gui.station_numtracks;
			int y = _settings_client.gui.station_platlength;
			if (_station_gui.axis == AXIS_X) std::swap(x, y);
			if (!_remove_button_clicked) {
				SetTileSelectSize(x, y);
			}
		}

		int rad = (_settings_game.station.modified_catchment) ? CA_TRAIN : CA_UNMODIFIED;

		if (_settings_client.gui.station_show_coverage) SetTileSelectBigSize(-rad, -rad, 2 * rad, 2 * rad);

		for (uint bits = 0; bits < 7; bits++) {
			bool disable = bits >= _settings_game.station.station_spread;
			if (statspec == nullptr) {
				this->SetWidgetDisabledState(bits + WID_BRAS_PLATFORM_NUM_1, disable);
				this->SetWidgetDisabledState(bits + WID_BRAS_PLATFORM_LEN_1, disable);
			} else {
				this->SetWidgetDisabledState(bits + WID_BRAS_PLATFORM_NUM_1, HasBit(statspec->disallowed_platforms, bits) || disable);
				this->SetWidgetDisabledState(bits + WID_BRAS_PLATFORM_LEN_1, HasBit(statspec->disallowed_lengths,   bits) || disable);
			}
		}

		this->DrawWidgets();

		if (this->IsShaded()) return;
		/* 'Accepts' and 'Supplies' texts. */
		Rect r = this->GetWidget<NWidgetBase>(WID_BRAS_COVERAGE_TEXTS)->GetCurrentRect();
		const int bottom = r.bottom;
		r.bottom = INT_MAX; // Allow overflow as we want to know the required height.
		if (statspec != nullptr) r.top = DrawBadgeNameList(r, statspec->badges, GrfSpecFeature::Stations);
		r.top = DrawStationCoverageAreaText(r, SCT_ALL, rad, false) + WidgetDimensions::scaled.vsep_normal;
		r.top = DrawStationCoverageAreaText(r, SCT_ALL, rad, true);
		/* Resize background if the window is too small.
		 * Never make the window smaller to avoid oscillating if the size change affects the acceptance.
		 * (This is the case, if making the window bigger moves the mouse into the window.) */
		if (r.top > bottom) {
			this->coverage_height += r.top - bottom;
			this->ReInit();
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_BRAS_PLATFORM_DIR_X:
			case WID_BRAS_PLATFORM_DIR_Y:
				size.width  = ScaleGUITrad(PREVIEW_WIDTH) + WidgetDimensions::scaled.fullbevel.Horizontal();
				size.height = ScaleGUITrad(PREVIEW_HEIGHT) + WidgetDimensions::scaled.fullbevel.Vertical();
				break;

			case WID_BRAS_COVERAGE_TEXTS:
				size.height = this->coverage_height;
				break;

			default:
				this->PickerWindow::UpdateWidgetSize(widget, size, padding, fill, resize);
				break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		DrawPixelInfo tmp_dpi;

		switch (widget) {
			case WID_BRAS_PLATFORM_DIR_X: {
				/* Set up a clipping area for the '/' station preview */
				Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
				if (FillDrawPixelInfo(&tmp_dpi, ir)) {
					AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
					int x = (ir.Width()  - ScaleSpriteTrad(PREVIEW_WIDTH)) / 2 + ScaleSpriteTrad(PREVIEW_LEFT);
					int y = (ir.Height() + ScaleSpriteTrad(PREVIEW_HEIGHT)) / 2 - ScaleSpriteTrad(PREVIEW_BOTTOM);
					if (!DrawStationTile(x, y, _cur_railtype, AXIS_X, _station_gui.sel_class, _station_gui.sel_type)) {
						StationPickerDrawSprite(x, y, StationType::Rail, _cur_railtype, INVALID_ROADTYPE, 2);
					}
				}
				break;
			}

			case WID_BRAS_PLATFORM_DIR_Y: {
				/* Set up a clipping area for the '\' station preview */
				Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
				if (FillDrawPixelInfo(&tmp_dpi, ir)) {
					AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
					int x = (ir.Width()  - ScaleSpriteTrad(PREVIEW_WIDTH)) / 2 + ScaleSpriteTrad(PREVIEW_LEFT);
					int y = (ir.Height() + ScaleSpriteTrad(PREVIEW_HEIGHT)) / 2 - ScaleSpriteTrad(PREVIEW_BOTTOM);
					if (!DrawStationTile(x, y, _cur_railtype, AXIS_Y, _station_gui.sel_class, _station_gui.sel_type)) {
						StationPickerDrawSprite(x, y, StationType::Rail, _cur_railtype, INVALID_ROADTYPE, 3);
					}
				}
				break;
			}

			default:
				this->PickerWindow::DrawWidget(r, widget);
				break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BRAS_PLATFORM_DIR_X:
			case WID_BRAS_PLATFORM_DIR_Y:
				this->RaiseWidget(WID_BRAS_PLATFORM_DIR_X + _station_gui.axis);
				_station_gui.axis = (Axis)(widget - WID_BRAS_PLATFORM_DIR_X);
				this->LowerWidget(WID_BRAS_PLATFORM_DIR_X + _station_gui.axis);
				SndClickBeep();
				this->SetDirty();
				CloseWindowById(WC_SELECT_STATION, 0);
				break;

			case WID_BRAS_PLATFORM_NUM_1:
			case WID_BRAS_PLATFORM_NUM_2:
			case WID_BRAS_PLATFORM_NUM_3:
			case WID_BRAS_PLATFORM_NUM_4:
			case WID_BRAS_PLATFORM_NUM_5:
			case WID_BRAS_PLATFORM_NUM_6:
			case WID_BRAS_PLATFORM_NUM_7: {
				this->RaiseWidget(WID_BRAS_PLATFORM_NUM_BEGIN + _settings_client.gui.station_numtracks);
				this->RaiseWidget(WID_BRAS_PLATFORM_DRAG_N_DROP);

				_settings_client.gui.station_numtracks = widget - WID_BRAS_PLATFORM_NUM_BEGIN;
				_settings_client.gui.station_dragdrop = false;

				const StationSpec *statspec = StationClass::Get(_station_gui.sel_class)->GetSpec(_station_gui.sel_type);
				if (statspec != nullptr && HasBit(statspec->disallowed_lengths, _settings_client.gui.station_platlength - 1)) {
					/* The previously selected number of platforms in invalid */
					for (uint i = 0; i < 7; i++) {
						if (!HasBit(statspec->disallowed_lengths, i)) {
							this->RaiseWidget(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN);
							_settings_client.gui.station_platlength = i + 1;
							break;
						}
					}
				}

				this->LowerWidget(_settings_client.gui.station_numtracks + WID_BRAS_PLATFORM_NUM_BEGIN);
				this->LowerWidget(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN);
				SndClickBeep();
				this->SetDirty();
				CloseWindowById(WC_SELECT_STATION, 0);
				break;
			}

			case WID_BRAS_PLATFORM_LEN_1:
			case WID_BRAS_PLATFORM_LEN_2:
			case WID_BRAS_PLATFORM_LEN_3:
			case WID_BRAS_PLATFORM_LEN_4:
			case WID_BRAS_PLATFORM_LEN_5:
			case WID_BRAS_PLATFORM_LEN_6:
			case WID_BRAS_PLATFORM_LEN_7: {
				this->RaiseWidget(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN);
				this->RaiseWidget(WID_BRAS_PLATFORM_DRAG_N_DROP);

				_settings_client.gui.station_platlength = widget - WID_BRAS_PLATFORM_LEN_BEGIN;
				_settings_client.gui.station_dragdrop = false;

				const StationSpec *statspec = StationClass::Get(_station_gui.sel_class)->GetSpec(_station_gui.sel_type);
				if (statspec != nullptr && HasBit(statspec->disallowed_platforms, _settings_client.gui.station_numtracks - 1)) {
					/* The previously selected number of tracks in invalid */
					for (uint i = 0; i < 7; i++) {
						if (!HasBit(statspec->disallowed_platforms, i)) {
							this->RaiseWidget(_settings_client.gui.station_numtracks + WID_BRAS_PLATFORM_NUM_BEGIN);
							_settings_client.gui.station_numtracks = i + 1;
							break;
						}
					}
				}

				this->LowerWidget(_settings_client.gui.station_numtracks + WID_BRAS_PLATFORM_NUM_BEGIN);
				this->LowerWidget(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN);
				SndClickBeep();
				this->SetDirty();
				CloseWindowById(WC_SELECT_STATION, 0);
				break;
			}

			case WID_BRAS_PLATFORM_DRAG_N_DROP: {
				_settings_client.gui.station_dragdrop ^= true;

				this->ToggleWidgetLoweredState(WID_BRAS_PLATFORM_DRAG_N_DROP);

				/* get the first allowed length/number of platforms */
				const StationSpec *statspec = StationClass::Get(_station_gui.sel_class)->GetSpec(_station_gui.sel_type);
				if (statspec != nullptr && HasBit(statspec->disallowed_lengths, _settings_client.gui.station_platlength - 1)) {
					for (uint i = 0; i < 7; i++) {
						if (!HasBit(statspec->disallowed_lengths, i)) {
							this->RaiseWidget(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN);
							_settings_client.gui.station_platlength = i + 1;
							break;
						}
					}
				}
				if (statspec != nullptr && HasBit(statspec->disallowed_platforms, _settings_client.gui.station_numtracks - 1)) {
					for (uint i = 0; i < 7; i++) {
						if (!HasBit(statspec->disallowed_platforms, i)) {
							this->RaiseWidget(_settings_client.gui.station_numtracks + WID_BRAS_PLATFORM_NUM_BEGIN);
							_settings_client.gui.station_numtracks = i + 1;
							break;
						}
					}
				}

				this->SetWidgetLoweredState(_settings_client.gui.station_numtracks + WID_BRAS_PLATFORM_NUM_BEGIN, !_settings_client.gui.station_dragdrop);
				this->SetWidgetLoweredState(_settings_client.gui.station_platlength + WID_BRAS_PLATFORM_LEN_BEGIN, !_settings_client.gui.station_dragdrop);
				SndClickBeep();
				this->SetDirty();
				CloseWindowById(WC_SELECT_STATION, 0);
				break;
			}

			case WID_BRAS_HIGHLIGHT_OFF:
			case WID_BRAS_HIGHLIGHT_ON:
				_settings_client.gui.station_show_coverage = (widget != WID_BRAS_HIGHLIGHT_OFF);

				this->SetWidgetLoweredState(WID_BRAS_HIGHLIGHT_OFF, !_settings_client.gui.station_show_coverage);
				this->SetWidgetLoweredState(WID_BRAS_HIGHLIGHT_ON, _settings_client.gui.station_show_coverage);
				SndClickBeep();
				this->SetDirty();
				SetViewportCatchmentStation(nullptr, true);
				break;

			default:
				this->PickerWindow::OnClick(pt, widget, click_count);
				break;
		}
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		CheckRedrawStationCoverage(this);
	}

	/**
	 * Handler for global hotkeys of the BuildRailStationWindow.
	 * @param hotkey Hotkey
	 * @return ES_HANDLED if hotkey was accepted.
	 */
	static EventState BuildRailStationGlobalHotkeys(int hotkey)
	{
		if (_game_mode == GM_MENU) return ES_NOT_HANDLED;
		Window *w = ShowStationBuilder(FindWindowById(WC_BUILD_TOOLBAR, TRANSPORT_RAIL));
		if (w == nullptr) return ES_NOT_HANDLED;
		return w->OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"buildrailstation", {
		Hotkey('F', "focus_filter_box", PCWHK_FOCUS_FILTER_BOX),
	}, BuildRailStationGlobalHotkeys};
};

static constexpr std::initializer_list<NWidgetPart> _nested_station_builder_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_STATION_BUILD_RAIL_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_DEFSIZEBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidgetFunction(MakePickerClassWidgets),
			NWidget(WWT_PANEL, Colours::DarkGreen),
				NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_picker, 0), SetPadding(WidgetDimensions::unscaled.picker),
					NWidget(WWT_LABEL, Colours::Invalid), SetMinimalSize(144, 11), SetFill(1, 0), SetStringTip(STR_STATION_BUILD_ORIENTATION),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
						NWidget(WWT_PANEL, Colours::Grey, WID_BRAS_PLATFORM_DIR_X), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_RAILROAD_ORIENTATION_TOOLTIP), EndContainer(),
						NWidget(WWT_PANEL, Colours::Grey, WID_BRAS_PLATFORM_DIR_Y), SetFill(0, 0), SetToolTip(STR_STATION_BUILD_RAILROAD_ORIENTATION_TOOLTIP), EndContainer(),
					EndContainer(),
					NWidget(WWT_LABEL, Colours::Invalid), SetMinimalSize(144, 11), SetFill(1, 0), SetStringTip(STR_STATION_BUILD_NUMBER_OF_TRACKS),
					NWidget(NWID_HORIZONTAL), SetPIPRatio(1, 0, 1),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_NUM_1), SetAspect(1.25f), SetStringTip(STR_BLACK_1, STR_STATION_BUILD_NUMBER_OF_TRACKS_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_NUM_2), SetAspect(1.25f), SetStringTip(STR_BLACK_2, STR_STATION_BUILD_NUMBER_OF_TRACKS_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_NUM_3), SetAspect(1.25f), SetStringTip(STR_BLACK_3, STR_STATION_BUILD_NUMBER_OF_TRACKS_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_NUM_4), SetAspect(1.25f), SetStringTip(STR_BLACK_4, STR_STATION_BUILD_NUMBER_OF_TRACKS_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_NUM_5), SetAspect(1.25f), SetStringTip(STR_BLACK_5, STR_STATION_BUILD_NUMBER_OF_TRACKS_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_NUM_6), SetAspect(1.25f), SetStringTip(STR_BLACK_6, STR_STATION_BUILD_NUMBER_OF_TRACKS_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_NUM_7), SetAspect(1.25f), SetStringTip(STR_BLACK_7, STR_STATION_BUILD_NUMBER_OF_TRACKS_TOOLTIP),
					EndContainer(),
					NWidget(WWT_LABEL, Colours::Invalid), SetMinimalSize(144, 11), SetFill(1, 0), SetStringTip(STR_STATION_BUILD_PLATFORM_LENGTH),
					NWidget(NWID_HORIZONTAL), SetPIPRatio(1, 0, 1),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_LEN_1), SetAspect(1.25f), SetStringTip(STR_BLACK_1, STR_STATION_BUILD_PLATFORM_LENGTH_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_LEN_2), SetAspect(1.25f), SetStringTip(STR_BLACK_2, STR_STATION_BUILD_PLATFORM_LENGTH_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_LEN_3), SetAspect(1.25f), SetStringTip(STR_BLACK_3, STR_STATION_BUILD_PLATFORM_LENGTH_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_LEN_4), SetAspect(1.25f), SetStringTip(STR_BLACK_4, STR_STATION_BUILD_PLATFORM_LENGTH_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_LEN_5), SetAspect(1.25f), SetStringTip(STR_BLACK_5, STR_STATION_BUILD_PLATFORM_LENGTH_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_LEN_6), SetAspect(1.25f), SetStringTip(STR_BLACK_6, STR_STATION_BUILD_PLATFORM_LENGTH_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_LEN_7), SetAspect(1.25f), SetStringTip(STR_BLACK_7, STR_STATION_BUILD_PLATFORM_LENGTH_TOOLTIP),
					EndContainer(),
					NWidget(NWID_HORIZONTAL), SetPIPRatio(1, 0, 1),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_PLATFORM_DRAG_N_DROP), SetMinimalSize(75, 12), SetStringTip(STR_STATION_BUILD_DRAG_DROP, STR_STATION_BUILD_DRAG_DROP_TOOLTIP),
					EndContainer(),
					NWidget(WWT_LABEL, Colours::Invalid), SetStringTip(STR_STATION_BUILD_COVERAGE_AREA_TITLE), SetFill(1, 0),
					NWidget(NWID_HORIZONTAL), SetPIPRatio(1, 0, 1),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_HIGHLIGHT_OFF), SetMinimalSize(60, 12), SetStringTip(STR_STATION_BUILD_COVERAGE_OFF, STR_STATION_BUILD_COVERAGE_AREA_OFF_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAS_HIGHLIGHT_ON), SetMinimalSize(60, 12), SetStringTip(STR_STATION_BUILD_COVERAGE_ON, STR_STATION_BUILD_COVERAGE_AREA_ON_TOOLTIP),
					EndContainer(),
					NWidget(WWT_EMPTY, Colours::Invalid, WID_BRAS_COVERAGE_TEXTS), SetFill(1, 1), SetResize(1, 0), SetMinimalTextLines(2, 0),
				EndContainer(),
			EndContainer(),
		EndContainer(),
		NWidgetFunction(MakePickerTypeWidgets),
	EndContainer(),
};

/** High level window description of the station-build window (default & newGRF) */
static WindowDesc _station_builder_desc(
	WindowPosition::Automatic, "build_station_rail", 0, 0,
	WC_BUILD_STATION, WC_BUILD_TOOLBAR,
	WindowDefaultFlag::Construction,
	_nested_station_builder_widgets,
	&BuildRailStationWindow::hotkeys
);

/**
 * Open station build window.
 * @param parent The parent window.
 * @return The created window.
 */
static Window *ShowStationBuilder(Window *parent)
{
	return new BuildRailStationWindow(_station_builder_desc, parent);
}

struct BuildSignalWindow : public PickerWindowBase {
private:
	Dimension sig_sprite_size{}; ///< Maximum size of signal GUI sprites.
	int sig_sprite_bottom_offset = 0; ///< Maximum extent of signal GUI sprite from reference point towards bottom.

	/**
	 * Draw dynamic a signal-sprite in a button in the signal GUI
	 * @param r The rectangle to draw the sprite in.
	 * @param image        the sprite to draw
	 */
	void DrawSignalSprite(const Rect &r, SpriteID image) const
	{
		Point offset;
		Dimension sprite_size = GetSpriteSize(image, &offset);
		Rect ir = r.Shrink(WidgetDimensions::scaled.imgbtn);
		int x = CentreBounds(ir.left, ir.right, sprite_size.width - offset.x) - offset.x; // centered
		int y = ir.top - sig_sprite_bottom_offset +
				(ir.Height() + sig_sprite_size.height) / 2; // aligned to bottom

		DrawSprite(image, PAL_NONE, x, y);
	}

	/** Show or hide buttons for non-path signals in the signal GUI */
	void SetSignalUIMode()
	{
		bool show_non_path_signals = (_settings_client.gui.signal_gui_mode == SIGNAL_GUI_ALL);

		this->GetWidget<NWidgetStacked>(WID_BS_BLOCK_SEL)->SetDisplayedPlane(show_non_path_signals ? 0 : SZSP_NONE);
		this->GetWidget<NWidgetStacked>(WID_BS_BLOCK_SPACER_SEL)->SetDisplayedPlane(show_non_path_signals ? 0 : SZSP_NONE);
	}

public:
	BuildSignalWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();
		this->SetSignalUIMode();
		this->FinishInitNested(TRANSPORT_RAIL);
		this->OnInvalidateData();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		_convert_signal_button = false;
		this->PickerWindowBase::Close();
	}

	void OnInit() override
	{
		/* Calculate maximum signal sprite size. */
		this->sig_sprite_size.width = 0;
		this->sig_sprite_size.height = 0;
		this->sig_sprite_bottom_offset = 0;
		const RailTypeInfo *rti = GetRailTypeInfo(_cur_railtype);
		for (uint type = SIGTYPE_BLOCK; type < SIGTYPE_END; type++) {
			for (uint variant = SIG_ELECTRIC; variant <= SIG_SEMAPHORE; variant++) {
				for (uint lowered = 0; lowered < 2; lowered++) {
					Point offset;
					Dimension sprite_size = GetSpriteSize(rti->gui_sprites.signals[type][variant][lowered], &offset);
					this->sig_sprite_bottom_offset = std::max<int>(this->sig_sprite_bottom_offset, sprite_size.height);
					this->sig_sprite_size.width = std::max<int>(this->sig_sprite_size.width, sprite_size.width - offset.x);
					this->sig_sprite_size.height = std::max<int>(this->sig_sprite_size.height, sprite_size.height - offset.y);
				}
			}
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_BS_DRAG_SIGNALS_DENSITY_LABEL) {
			/* Two digits for signals density. */
			size.width = std::max(size.width, 2 * GetDigitWidth() + padding.width + WidgetDimensions::scaled.framerect.Horizontal());
		} else if (IsInsideMM(widget, WID_BS_SEMAPHORE_NORM, WID_BS_ELECTRIC_PBS_OWAY + 1)) {
			size.width = std::max(size.width, this->sig_sprite_size.width + padding.width);
			size.height = std::max(size.height, this->sig_sprite_size.height + padding.height);
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_BS_DRAG_SIGNALS_DENSITY_LABEL:
				return GetString(STR_JUST_INT, _settings_client.gui.drag_signals_density);

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (IsInsideMM(widget, WID_BS_SEMAPHORE_NORM, WID_BS_ELECTRIC_PBS_OWAY + 1)) {
			/* Extract signal from widget number. */
			int type = (widget - WID_BS_SEMAPHORE_NORM) % SIGTYPE_END;
			int var = SIG_SEMAPHORE - (widget - WID_BS_SEMAPHORE_NORM) / SIGTYPE_END; // SignalVariant order is reversed compared to the widgets.
			SpriteID sprite = GetRailTypeInfo(_cur_railtype)->gui_sprites.signals[type][var][this->IsWidgetLowered(widget)];

			this->DrawSignalSprite(r, sprite);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BS_SEMAPHORE_NORM:
			case WID_BS_SEMAPHORE_ENTRY:
			case WID_BS_SEMAPHORE_EXIT:
			case WID_BS_SEMAPHORE_COMBO:
			case WID_BS_SEMAPHORE_PBS:
			case WID_BS_SEMAPHORE_PBS_OWAY:
			case WID_BS_ELECTRIC_NORM:
			case WID_BS_ELECTRIC_ENTRY:
			case WID_BS_ELECTRIC_EXIT:
			case WID_BS_ELECTRIC_COMBO:
			case WID_BS_ELECTRIC_PBS:
			case WID_BS_ELECTRIC_PBS_OWAY:
				this->RaiseWidget((_cur_signal_variant == SIG_ELECTRIC ? WID_BS_ELECTRIC_NORM : WID_BS_SEMAPHORE_NORM) + _cur_signal_type);

				_cur_signal_type = (SignalType)((uint)((widget - WID_BS_SEMAPHORE_NORM) % (SIGTYPE_LAST + 1)));
				_cur_signal_variant = widget >= WID_BS_ELECTRIC_NORM ? SIG_ELECTRIC : SIG_SEMAPHORE;

				/* Update default (last-used) signal type in config file. */
				_settings_client.gui.default_signal_type = _cur_signal_type;

				/* If 'remove' button of rail build toolbar is active, disable it. */
				if (_remove_button_clicked) {
					Window *w = FindWindowById(WC_BUILD_TOOLBAR, TRANSPORT_RAIL);
					if (w != nullptr) ToggleRailButton_Remove(w);
				}

				SndClickBeep();
				break;

			case WID_BS_CONVERT:
				_convert_signal_button = !_convert_signal_button;
				SndClickBeep();
				break;

			case WID_BS_DRAG_SIGNALS_DENSITY_DECREASE:
				if (_settings_client.gui.drag_signals_density > 1) {
					_settings_client.gui.drag_signals_density--;
					SetWindowDirty(WC_GAME_OPTIONS, WN_GAME_OPTIONS_GAME_SETTINGS);
				}
				break;

			case WID_BS_DRAG_SIGNALS_DENSITY_INCREASE:
				if (_settings_client.gui.drag_signals_density < 20) {
					_settings_client.gui.drag_signals_density++;
					SetWindowDirty(WC_GAME_OPTIONS, WN_GAME_OPTIONS_GAME_SETTINGS);
				}
				break;

			default: break;
		}

		this->InvalidateData();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->LowerWidget((_cur_signal_variant == SIG_ELECTRIC ? WID_BS_ELECTRIC_NORM : WID_BS_SEMAPHORE_NORM) + _cur_signal_type);

		this->SetWidgetLoweredState(WID_BS_CONVERT, _convert_signal_button);

		this->SetWidgetDisabledState(WID_BS_DRAG_SIGNALS_DENSITY_DECREASE, _settings_client.gui.drag_signals_density == 1);
		this->SetWidgetDisabledState(WID_BS_DRAG_SIGNALS_DENSITY_INCREASE, _settings_client.gui.drag_signals_density == 20);
	}
};

/** Nested widget definition of the build signal window */
static constexpr std::initializer_list<NWidgetPart> _nested_signal_builder_widgets = {
	/* Title bar and buttons. */
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BS_CAPTION), SetStringTip(STR_BUILD_SIGNAL_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),

	/* Container for both signal groups, spacers, and convert/autofill buttons. */
	NWidget(NWID_HORIZONTAL),
		/* Block signals (can be hidden). */
		NWidget(NWID_SELECTION, Colours::Invalid, WID_BS_BLOCK_SEL),
			NWidget(NWID_VERTICAL, NWidContainerFlag::EqualSize),
				/* Semaphore block signals. */
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_SEMAPHORE_NORM), SetToolTip(STR_BUILD_SIGNAL_SEMAPHORE_NORM_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_SEMAPHORE_ENTRY), SetToolTip(STR_BUILD_SIGNAL_SEMAPHORE_ENTRY_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_SEMAPHORE_EXIT), SetToolTip(STR_BUILD_SIGNAL_SEMAPHORE_EXIT_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_SEMAPHORE_COMBO), SetToolTip(STR_BUILD_SIGNAL_SEMAPHORE_COMBO_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
				EndContainer(),
				/* Electric block signals. */
				NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_ELECTRIC_NORM), SetToolTip(STR_BUILD_SIGNAL_ELECTRIC_NORM_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_ELECTRIC_ENTRY), SetToolTip(STR_BUILD_SIGNAL_ELECTRIC_ENTRY_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_ELECTRIC_EXIT), SetToolTip(STR_BUILD_SIGNAL_ELECTRIC_EXIT_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
					NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_ELECTRIC_COMBO), SetToolTip(STR_BUILD_SIGNAL_ELECTRIC_COMBO_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
				EndContainer(),
			EndContainer(),
		EndContainer(),

		/* Divider (only shown if block signals visible). */
		NWidget(NWID_SELECTION, Colours::Invalid, WID_BS_BLOCK_SPACER_SEL),
			NWidget(WWT_PANEL, Colours::DarkGreen), SetFill(0, 0), SetToolbarSpacerMinimalSize(), EndContainer(),
		EndContainer(),

		/* Path signals. */
		NWidget(NWID_VERTICAL, NWidContainerFlag::EqualSize),
			/* Semaphore path signals. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_SEMAPHORE_PBS), SetToolTip(STR_BUILD_SIGNAL_SEMAPHORE_PBS_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
				NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_SEMAPHORE_PBS_OWAY), SetToolTip(STR_BUILD_SIGNAL_SEMAPHORE_PBS_OWAY_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
			EndContainer(),
			/* Electric path signals. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_ELECTRIC_PBS), SetToolTip(STR_BUILD_SIGNAL_ELECTRIC_PBS_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
				NWidget(WWT_PANEL, Colours::DarkGreen, WID_BS_ELECTRIC_PBS_OWAY), SetToolTip(STR_BUILD_SIGNAL_ELECTRIC_PBS_OWAY_TOOLTIP), SetToolbarMinimalSize(1), EndContainer(),
			EndContainer(),
		EndContainer(),

		/* Convert/autofill buttons. */
		NWidget(NWID_VERTICAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BS_CONVERT), SetSpriteTip(SPR_IMG_SIGNAL_CONVERT, STR_BUILD_SIGNAL_CONVERT_TOOLTIP), SetFill(0, 1),
			NWidget(WWT_PANEL, Colours::DarkGreen), SetToolTip(STR_BUILD_SIGNAL_DRAG_SIGNALS_DENSITY_TOOLTIP), SetFill(0, 1),
				NWidget(NWID_VERTICAL), SetPadding(2), SetPIPRatio(1, 0, 1),
					NWidget(WWT_LABEL, Colours::Invalid, WID_BS_DRAG_SIGNALS_DENSITY_LABEL), SetToolTip(STR_BUILD_SIGNAL_DRAG_SIGNALS_DENSITY_TOOLTIP), SetTextStyle(TC_ORANGE), SetFill(1, 1),
					NWidget(NWID_HORIZONTAL), SetPIPRatio(1, 0, 1),
						NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_BS_DRAG_SIGNALS_DENSITY_DECREASE), SetMinimalSize(9, 12), SetArrowWidgetTypeTip(ArrowWidgetType::Decrease, STR_BUILD_SIGNAL_DRAG_SIGNALS_DENSITY_DECREASE_TOOLTIP),
						NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_BS_DRAG_SIGNALS_DENSITY_INCREASE), SetMinimalSize(9, 12), SetArrowWidgetTypeTip(ArrowWidgetType::Increase, STR_BUILD_SIGNAL_DRAG_SIGNALS_DENSITY_INCREASE_TOOLTIP),
					EndContainer(),
				EndContainer(),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

/** Signal selection window description */
static WindowDesc _signal_builder_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WC_BUILD_SIGNAL, WC_BUILD_TOOLBAR,
	WindowDefaultFlag::Construction,
	_nested_signal_builder_widgets
);

/**
 * Open the signal selection window
 * @param parent The parent window.
 */
static void ShowSignalBuilder(Window *parent)
{
	new BuildSignalWindow(_signal_builder_desc, parent);
}

struct BuildRailDepotWindow : public PickerWindowBase {
	BuildRailDepotWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->InitNested(TRANSPORT_RAIL);
		this->LowerWidget(WID_BRAD_DEPOT_NE + _build_depot_direction);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (!IsInsideMM(widget, WID_BRAD_DEPOT_NE, WID_BRAD_DEPOT_NW + 1)) return;

		size.width  = ScaleGUITrad(64) + WidgetDimensions::scaled.fullbevel.Horizontal();
		size.height = ScaleGUITrad(48) + WidgetDimensions::scaled.fullbevel.Vertical();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (!IsInsideMM(widget, WID_BRAD_DEPOT_NE, WID_BRAD_DEPOT_NW + 1)) return;

		DrawPixelInfo tmp_dpi;
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		if (FillDrawPixelInfo(&tmp_dpi, ir)) {
			AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
			int x = (ir.Width()  - ScaleSpriteTrad(64)) / 2 + ScaleSpriteTrad(31);
			int y = (ir.Height() + ScaleSpriteTrad(48)) / 2 - ScaleSpriteTrad(31);
			DrawTrainDepotSprite(x, y, widget - WID_BRAD_DEPOT_NE + DIAGDIR_NE, _cur_railtype);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BRAD_DEPOT_NE:
			case WID_BRAD_DEPOT_SE:
			case WID_BRAD_DEPOT_SW:
			case WID_BRAD_DEPOT_NW:
				this->RaiseWidget(WID_BRAD_DEPOT_NE + _build_depot_direction);
				_build_depot_direction = (DiagDirection)(widget - WID_BRAD_DEPOT_NE);
				this->LowerWidget(WID_BRAD_DEPOT_NE + _build_depot_direction);
				SndClickBeep();
				this->SetDirty();
				break;
		}
	}
};

/** Nested widget definition of the build rail depot window */
static constexpr std::initializer_list<NWidgetPart> _nested_build_depot_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_BUILD_DEPOT_TRAIN_ORIENTATION_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAD_DEPOT_NW), SetToolTip(STR_BUILD_DEPOT_TRAIN_ORIENTATION_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAD_DEPOT_SW), SetToolTip(STR_BUILD_DEPOT_TRAIN_ORIENTATION_TOOLTIP),
			EndContainer(),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAD_DEPOT_NE), SetToolTip(STR_BUILD_DEPOT_TRAIN_ORIENTATION_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BRAD_DEPOT_SE), SetToolTip(STR_BUILD_DEPOT_TRAIN_ORIENTATION_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

/** Window definition for the build rail depot window. */
static WindowDesc _build_depot_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WC_BUILD_DEPOT, WC_BUILD_TOOLBAR,
	WindowDefaultFlag::Construction,
	_nested_build_depot_widgets
);

static void ShowBuildTrainDepotPicker(Window *parent)
{
	new BuildRailDepotWindow(_build_depot_desc, parent);
}

class WaypointPickerCallbacks : public PickerCallbacksNewGRFClass<StationClass> {
public:
	WaypointPickerCallbacks() : PickerCallbacksNewGRFClass<StationClass>("fav_waypoints") {}

	GrfSpecFeature GetFeature() const override { return GrfSpecFeature::Stations; }

	StringID GetClassTooltip() const override { return STR_PICKER_WAYPOINT_CLASS_TOOLTIP; }
	StringID GetTypeTooltip() const override { return STR_PICKER_WAYPOINT_TYPE_TOOLTIP; }
	StringID GetCollectionTooltip() const override { return STR_PICKER_WAYPOINT_COLLECTION_TOOLTIP; }

	bool IsActive() const override
	{
		for (const auto &cls : StationClass::Classes()) {
			if (!IsWaypointClass(cls)) continue;
			for (const auto *spec : cls.Specs()) {
				if (spec != nullptr) return true;
			}
		}
		return false;
	}

	bool HasClassChoice() const override
	{
		return std::ranges::count_if(StationClass::Classes(), [](const auto &cls) { return IsWaypointClass(cls); }) > 1;
	}

	void Close(int) override { ResetObjectToPlace(); }
	int GetSelectedClass() const override { return _waypoint_gui.sel_class.base(); }
	void SetSelectedClass(int id) const override { _waypoint_gui.sel_class = this->GetClassIndex(id); }

	StringID GetClassName(int id) const override
	{
		const auto *sc = GetClass(id);
		if (!IsWaypointClass(*sc)) return INVALID_STRING_ID;
		return sc->name;
	}

	int GetSelectedType() const override { return _waypoint_gui.sel_type; }
	void SetSelectedType(int id) const override { _waypoint_gui.sel_type = id; }

	StringID GetTypeName(int cls_id, int id) const override
	{
		const auto *spec = this->GetSpec(cls_id, id);
		return (spec == nullptr) ? STR_STATION_CLASS_WAYP_WAYPOINT : spec->name;
	}

	std::span<const BadgeID> GetTypeBadges(int cls_id, int id) const override
	{
		const auto *spec = this->GetSpec(cls_id, id);
		if (spec == nullptr) return {};
		return spec->badges;
	}

	bool IsTypeAvailable(int cls_id, int id) const override
	{
		return IsStationAvailable(this->GetSpec(cls_id, id));
	}

	void DrawType(int x, int y, int cls_id, int id) const override
	{
		DrawWaypointSprite(x, y, this->GetClassIndex(cls_id), id, _cur_railtype);
	}

	void FillUsedItems(std::set<PickerItem> &items) override
	{
		bool default_added = false;
		for (const Waypoint *wp : Waypoint::Iterate()) {
			if (wp->owner != _local_company || HasBit(wp->waypoint_flags, WPF_ROAD)) continue;
			if (!default_added && StationUsesDefaultType(wp)) {
				items.insert({0, 0, STAT_CLASS_WAYP.base(), 0});
				default_added = true;
			}
			for (const auto &sm : wp->speclist) {
				if (sm.spec == nullptr) continue;
				items.insert({sm.grfid, sm.localidx, sm.spec->class_index.base(), sm.spec->index});
			}
		}
	}

	static WaypointPickerCallbacks instance;
};
/* static */ WaypointPickerCallbacks WaypointPickerCallbacks::instance;

struct BuildRailWaypointWindow : public PickerWindow {
	BuildRailWaypointWindow(WindowDesc &desc, Window *parent) : PickerWindow(desc, parent, TRANSPORT_RAIL, WaypointPickerCallbacks::instance)
	{
		this->ConstructWindow();
	}

	static inline HotkeyList hotkeys{"buildrailwaypoint", {
		Hotkey('F', "focus_filter_box", PCWHK_FOCUS_FILTER_BOX),
	}};
};

/** Nested widget definition for the build NewGRF rail waypoint window */
static constexpr std::initializer_list<NWidgetPart> _nested_build_waypoint_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_WAYPOINT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_DEFSIZEBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidgetFunction(MakePickerClassWidgets),
		NWidgetFunction(MakePickerTypeWidgets),
	EndContainer(),
};

/** Window definition for the build rail waypoint window. */
static WindowDesc _build_waypoint_desc(
	WindowPosition::Automatic, "build_waypoint", 0, 0,
	WC_BUILD_WAYPOINT, WC_BUILD_TOOLBAR,
	WindowDefaultFlag::Construction,
	_nested_build_waypoint_widgets,
	&BuildRailWaypointWindow::hotkeys
);

static void ShowBuildWaypointPicker(Window *parent)
{
	if (!WaypointPickerCallbacks::instance.IsActive()) return;
	new BuildRailWaypointWindow(_build_waypoint_desc, parent);
}

/**
 * Initialize rail building GUI settings
 */
void InitializeRailGui()
{
	_build_depot_direction = DIAGDIR_NW;
	_station_gui.sel_class = STAT_CLASS_DFLT;
	_station_gui.sel_type = 0;
	_waypoint_gui.sel_class = STAT_CLASS_WAYP;
	_waypoint_gui.sel_type = 0;
}

/**
 * Re-initialize rail-build toolbar after toggling support for electric trains
 * @param disable Boolean whether electric trains are disabled (removed from the game)
 */
void ReinitGuiAfterToggleElrail(bool disable)
{
	if (disable && _last_built_railtype == RAILTYPE_ELECTRIC) {
		_last_built_railtype = _cur_railtype = RAILTYPE_RAIL;
		BuildRailToolbarWindow *w = dynamic_cast<BuildRailToolbarWindow *>(FindWindowById(WC_BUILD_TOOLBAR, TRANSPORT_RAIL));
		if (w != nullptr) w->ModifyRailType(_cur_railtype);
	}
	MarkWholeScreenDirty();
}

/** Set the initial (default) railtype to use */
void SetDefaultRailGui()
{
	if (_local_company == COMPANY_SPECTATOR || !Company::IsValidID(_local_company)) return;

	RailType rt;
	switch (_settings_client.gui.default_rail_type) {
		case 2: {
			/* Find the most used rail type */
			std::array<uint, RAILTYPE_END> count{};
			for (const auto t : Map::Iterate()) {
				if ((IsTileType(t, TileType::Railway) || IsLevelCrossingTile(t) || HasStationTileRail(t) ||
						(IsTileType(t, TileType::TunnelBridge) && GetTunnelBridgeTransportType(t) == TRANSPORT_RAIL)) && IsTileOwner(t, _local_company)) {
					count[GetRailType(t)]++;
				}
			}

			rt = static_cast<RailType>(std::distance(std::begin(count), std::ranges::max_element(count)));
			if (count[rt] > 0) break;

			/* No rail, just get the first available one */
			[[fallthrough]];
		}
		case 0: {
			/* Use first available type */
			std::vector<RailType>::const_iterator it = std::find_if(_sorted_railtypes.begin(), _sorted_railtypes.end(),
					[](RailType r) { return HasRailTypeAvail(_local_company, r); });
			rt = it != _sorted_railtypes.end() ? *it : RAILTYPE_BEGIN;
			break;
		}
		case 1: {
			/* Use last available type */
			std::vector<RailType>::const_reverse_iterator it = std::find_if(_sorted_railtypes.rbegin(), _sorted_railtypes.rend(),
					[](RailType r){ return HasRailTypeAvail(_local_company, r); });
			rt = it != _sorted_railtypes.rend() ? *it : RAILTYPE_BEGIN;
			break;
		}
		default:
			NOT_REACHED();
	}

	_last_built_railtype = _cur_railtype = rt;
	BuildRailToolbarWindow *w = dynamic_cast<BuildRailToolbarWindow *>(FindWindowById(WC_BUILD_TOOLBAR, TRANSPORT_RAIL));
	if (w != nullptr) w->ModifyRailType(_cur_railtype);
}

/**
 * Updates the current signal variant used in the signal GUI
 * to the one adequate to current year.
 */
void ResetSignalVariant(int32_t)
{
	SignalVariant new_variant = (TimerGameCalendar::year < _settings_client.gui.semaphore_build_before ? SIG_SEMAPHORE : SIG_ELECTRIC);

	if (new_variant != _cur_signal_variant) {
		Window *w = FindWindowById(WC_BUILD_SIGNAL, 0);
		if (w != nullptr) {
			w->SetDirty();
			w->RaiseWidget((_cur_signal_variant == SIG_ELECTRIC ? WID_BS_ELECTRIC_NORM : WID_BS_SEMAPHORE_NORM) + _cur_signal_type);
		}
		_cur_signal_variant = new_variant;
	}
}

/** Yearly time to check whether to set the signal variant to electric signals. */
static const IntervalTimer<TimerGameCalendar> _check_reset_signal{{TimerGameCalendar::Trigger::Year, TimerGameCalendar::Priority::None}, [](auto)
{
	if (TimerGameCalendar::year != _settings_client.gui.semaphore_build_before) return;

	ResetSignalVariant();
}};

/**
 * Resets the rail GUI - sets default railtype to build
 * and resets the signal GUI
 */
void InitializeRailGUI()
{
	SetDefaultRailGui();

	_convert_signal_button = false;
	_cur_signal_type = _settings_client.gui.default_signal_type;
	ResetSignalVariant();
}

/**
 * Create a drop down list for all the rail types of the local company.
 * @param for_replacement Whether this list is for the replacement window.
 * @param all_option Whether to add an 'all types' item.
 * @return The populated and sorted #DropDownList.
 */
DropDownList GetRailTypeDropDownList(bool for_replacement, bool all_option)
{
	RailTypes used_railtypes;
	RailTypes avail_railtypes;

	const Company *c = Company::Get(_local_company);

	/* Find the used railtypes. */
	if (for_replacement) {
		avail_railtypes = GetCompanyRailTypes(c->index, false);
		used_railtypes  = GetRailTypes(false);
	} else {
		avail_railtypes = c->avail_railtypes;
		used_railtypes  = GetRailTypes(true);
	}

	DropDownList list;

	if (all_option) {
		list.push_back(MakeDropDownListStringItem(STR_REPLACE_ALL_RAILTYPE, INVALID_RAILTYPE));
	}

	Dimension d = { 0, 0 };
	/* Get largest icon size, to ensure text is aligned on each menu item. */
	if (!for_replacement) {
		used_railtypes.Reset(_railtypes_hidden_mask);
		for (const auto &rt : _sorted_railtypes) {
			if (!used_railtypes.Test(rt)) continue;
			const RailTypeInfo *rti = GetRailTypeInfo(rt);
			d = maxdim(d, GetSpriteSize(rti->gui_sprites.build_x_rail));
		}
	}

	/* Shared list so that each item can take ownership. */
	auto badge_class_list = std::make_shared<GUIBadgeClasses>(GrfSpecFeature::RailTypes);

	for (const auto &rt : _sorted_railtypes) {
		/* If it's not used ever, don't show it to the user. */
		if (!used_railtypes.Test(rt)) continue;

		const RailTypeInfo *rti = GetRailTypeInfo(rt);

		if (for_replacement) {
			list.push_back(MakeDropDownListBadgeItem(badge_class_list, rti->badges, GrfSpecFeature::RailTypes, rti->introduction_date, GetString(rti->strings.replace_text), rt, !avail_railtypes.Test(rt)));
		} else {
			std::string str = rti->max_speed > 0
				? GetString(STR_TOOLBAR_RAILTYPE_VELOCITY, rti->strings.menu_text, rti->max_speed)
				: GetString(rti->strings.menu_text);
			list.push_back(MakeDropDownListBadgeIconItem(badge_class_list, rti->badges, GrfSpecFeature::RailTypes, rti->introduction_date, RailBuildCost(rt), d, rti->gui_sprites.build_x_rail, PAL_NONE, std::move(str), rt, !avail_railtypes.Test(rt)));
		}
	}

	if (list.empty()) {
		/* Empty dropdowns are not allowed */
		list.push_back(MakeDropDownListStringItem(STR_NONE, INVALID_RAILTYPE, true));
	}

	return list;
}
