/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file main_gui.cpp Handling of the main viewport. */

#include "stdafx.h"
#include "currency.h"
#include "spritecache.h"
#include "window_gui.h"
#include "window_func.h"
#include "textbuf_gui.h"
#include "viewport_func.h"
#include "command_func.h"
#include "console_gui.h"
#include "progress.h"
#include "transparency_gui.h"
#include "map_func.h"
#include "fileio_func.h"
#include "sound_func.h"
#include "transparency.h"
#include "strings_func.h"
#include "zoom_func.h"
#include "company_base.h"
#include "company_func.h"
#include "toolbar_gui.h"
#include "statusbar_gui.h"
#include "linkgraph/linkgraph_gui.h"
#include "tilehighlight_func.h"
#include "hotkeys.h"
#include "error.h"
#include "news_gui.h"
#include "misc_cmd.h"
#include "terraform_cmd.h"
#include "newgrf_station.h"
#include "rail_cmd.h"
#include "rail_map.h"
#include "station_cmd.h"
#include "station_map.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "tilearea_type.h"
#include "tile_map.h"
#include "track_func.h"

#include "saveload/saveload.h"

#include "widgets/main_widget.h"

#include "network/network.h"
#include "network/network_func.h"
#include "network/network_gui.h"
#include "network/network_base.h"

#include "table/sprites.h"
#include "table/strings.h"

#include <fstream>

#include "safeguards.h"

enum class ViewportClipboardMode : uint8_t {
	None,
	Copy,
	Paste,
};

struct ViewportClipboardRail {
	int16_t x;
	int16_t y;
	RailType railtype;
	TrackBits tracks;
};

struct ViewportClipboardSignal {
	int16_t x;
	int16_t y;
	Track track;
	SignalType sigtype;
	SignalVariant sigvar;
	uint8_t signals;
};

struct ViewportClipboardRailStation {
	int16_t x;
	int16_t y;
	RailType railtype;
	Axis axis;
};

struct ViewportClipboardRailStationArea {
	int16_t x;
	int16_t y;
	uint16_t w;
	uint16_t h;
	RailType railtype;
	Axis axis;
};

struct ViewportClipboard {
	std::vector<ViewportClipboardRail> rails;
	std::vector<ViewportClipboardSignal> signals;
	std::vector<ViewportClipboardRailStation> rail_stations;
	std::vector<ViewportClipboardRailStationArea> rail_station_areas;
	uint16_t w = 1;
	uint16_t h = 1;

	bool IsEmpty() const
	{
		return this->rails.empty() && this->signals.empty() && this->rail_stations.empty() && this->rail_station_areas.empty();
	}
};

static ViewportClipboardMode _viewport_clipboard_mode = ViewportClipboardMode::None;
static ViewportClipboard _viewport_clipboard;
static uint8_t _viewport_clipboard_rotation = 0;

struct ViewportClipboardStationGroup {
	StationID station_id;
	RailType railtype;
	Axis axis;
	TileArea area;
	std::vector<TileIndex> tiles;
};

static Dimension GetRotatedClipboardSize()
{
	return (_viewport_clipboard_rotation % 2) == 0 ?
			Dimension{_viewport_clipboard.w, _viewport_clipboard.h} :
			Dimension{_viewport_clipboard.h, _viewport_clipboard.w};
}

static void RotateClipboardArea(int16_t &x, int16_t &y, uint16_t &w, uint16_t &h)
{
	int16_t old_x = x;
	int16_t old_y = y;
	uint16_t old_w = w;
	uint16_t old_h = h;

	switch (_viewport_clipboard_rotation % 4) {
		case 0:
			break;

		case 1:
			x = static_cast<int16_t>(_viewport_clipboard.h - old_y - old_h);
			y = old_x;
			w = old_h;
			h = old_w;
			break;

		case 2:
			x = static_cast<int16_t>(_viewport_clipboard.w - old_x - old_w);
			y = static_cast<int16_t>(_viewport_clipboard.h - old_y - old_h);
			break;

		case 3:
			x = old_y;
			y = static_cast<int16_t>(_viewport_clipboard.w - old_x - old_w);
			w = old_h;
			h = old_w;
			break;
	}
}

static Axis RotateClipboardAxis(Axis axis)
{
	return (_viewport_clipboard_rotation % 2) == 0 ? axis : OtherAxis(axis);
}

static Track RotateClipboardTrackClockwise(Track track)
{
	static constexpr Track clockwise_track[] = {
		TRACK_Y,     // TRACK_X
		TRACK_X,     // TRACK_Y
		TRACK_LEFT,  // TRACK_UPPER
		TRACK_RIGHT, // TRACK_LOWER
		TRACK_LOWER, // TRACK_LEFT
		TRACK_UPPER, // TRACK_RIGHT
	};
	static_assert(std::size(clockwise_track) == TRACK_END);

	return clockwise_track[track];
}

static Track RotateClipboardTrack(Track track)
{
	for (uint8_t i = 0; i < _viewport_clipboard_rotation % 4; i++) {
		track = RotateClipboardTrackClockwise(track);
	}

	return track;
}

static TrackBits RotateClipboardTrackBits(TrackBits tracks)
{
	for (uint8_t i = 0; i < _viewport_clipboard_rotation % 4; i++) {
		TrackBits rotated = TRACK_BIT_NONE;
		for (Track track = TRACK_BEGIN; track != TRACK_END; track++) {
			if (!HasTrack(tracks, track)) continue;
			rotated |= TrackToTrackBits(RotateClipboardTrackClockwise(track));
		}
		tracks = rotated;
	}

	return tracks;
}

static Trackdir RotateClipboardTrackdir(Trackdir trackdir)
{
	Track track = TrackdirToTrack(trackdir);
	DiagDirection exitdir = TrackdirToExitdir(trackdir);

	for (uint8_t i = 0; i < _viewport_clipboard_rotation % 4; i++) {
		track = RotateClipboardTrackClockwise(track);
		exitdir = ChangeDiagDir(exitdir, DIAGDIRDIFF_90LEFT);
	}

	return TrackExitdirToTrackdir(track, exitdir);
}

static uint8_t RotateClipboardSignalBits(Track track, uint8_t signals)
{
	uint8_t rotated = 0;
	for (Trackdir trackdir : {TrackToTrackdir(track), ReverseTrackdir(TrackToTrackdir(track))}) {
		if ((signals & SignalAlongTrackdir(trackdir)) == 0) continue;
		rotated |= SignalAlongTrackdir(RotateClipboardTrackdir(trackdir));
	}
	return rotated;
}

static std::string GetViewportInfrastructureBlueprintFilename(uint slot)
{
	return fmt::format("{}viewport-blueprint-{}.txt", _personal_dir, slot);
}

static bool SaveViewportInfrastructureBlueprint(uint slot)
{
	std::ofstream os(OTTD2FS(GetViewportInfrastructureBlueprintFilename(slot)).c_str());
	if (os.fail()) return false;

	os << "OTTD_VIEWPORT_BLUEPRINT 1\n";
	os << "SIZE " << _viewport_clipboard.w << ' ' << _viewport_clipboard.h << '\n';

	os << "RAILS " << _viewport_clipboard.rails.size() << '\n';
	for (const ViewportClipboardRail &rail : _viewport_clipboard.rails) {
		os << rail.x << ' ' << rail.y << ' ' << static_cast<uint>(rail.railtype) << ' ' << static_cast<uint>(rail.tracks) << '\n';
	}

	os << "SIGNALS " << _viewport_clipboard.signals.size() << '\n';
	for (const ViewportClipboardSignal &signal : _viewport_clipboard.signals) {
		os << signal.x << ' ' << signal.y << ' ' << static_cast<uint>(signal.track) << ' ' << static_cast<uint>(signal.sigtype) << ' '
				<< static_cast<uint>(signal.sigvar) << ' ' << static_cast<uint>(signal.signals) << '\n';
	}

	os << "STATION_AREAS " << _viewport_clipboard.rail_station_areas.size() << '\n';
	for (const ViewportClipboardRailStationArea &station : _viewport_clipboard.rail_station_areas) {
		os << station.x << ' ' << station.y << ' ' << station.w << ' ' << station.h << ' ' << static_cast<uint>(station.railtype) << ' '
				<< static_cast<uint>(station.axis) << '\n';
	}

	os << "STATIONS " << _viewport_clipboard.rail_stations.size() << '\n';
	for (const ViewportClipboardRailStation &station : _viewport_clipboard.rail_stations) {
		os << station.x << ' ' << station.y << ' ' << static_cast<uint>(station.railtype) << ' ' << static_cast<uint>(station.axis) << '\n';
	}

	return os.good();
}

static bool LoadViewportInfrastructureBlueprint(uint slot)
{
	std::ifstream is(OTTD2FS(GetViewportInfrastructureBlueprintFilename(slot)).c_str());
	if (is.fail()) return false;

	std::string marker;
	uint version;
	if (!(is >> marker >> version) || marker != "OTTD_VIEWPORT_BLUEPRINT" || version != 1) return false;

	ViewportClipboard clipboard;
	auto read_section = [&](const char *expected_name, size_t &count) {
		std::string section_name;
		return static_cast<bool>(is >> section_name >> count) && section_name == expected_name;
	};

	std::string size_name;
	uint width;
	uint height;
	if (!(is >> size_name >> width >> height) || size_name != "SIZE" || width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX) return false;
	clipboard.w = static_cast<uint16_t>(width);
	clipboard.h = static_cast<uint16_t>(height);

	size_t count;
	if (!read_section("RAILS", count)) return false;
	for (size_t i = 0; i < count; i++) {
		int x, y, railtype, tracks;
		if (!(is >> x >> y >> railtype >> tracks)) return false;
		if (railtype < RAILTYPE_BEGIN || railtype >= RAILTYPE_END || tracks < TRACK_BIT_NONE || tracks > TRACK_BIT_MASK) return false;
		clipboard.rails.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<RailType>(railtype), static_cast<TrackBits>(tracks)});
	}

	if (!read_section("SIGNALS", count)) return false;
	for (size_t i = 0; i < count; i++) {
		int x, y, track, sigtype, sigvar, signals;
		if (!(is >> x >> y >> track >> sigtype >> sigvar >> signals)) return false;
		if (!IsValidTrack(static_cast<Track>(track)) || sigtype < SIGTYPE_BLOCK || sigtype > SIGTYPE_LAST || sigvar < SIG_ELECTRIC || sigvar > SIG_SEMAPHORE) return false;
		if ((signals & ~SignalOnTrack(static_cast<Track>(track))) != 0) return false;
		clipboard.signals.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<Track>(track),
				static_cast<SignalType>(sigtype), static_cast<SignalVariant>(sigvar), static_cast<uint8_t>(signals)});
	}

	if (!read_section("STATION_AREAS", count)) return false;
	for (size_t i = 0; i < count; i++) {
		int x, y, w, h, railtype, axis;
		if (!(is >> x >> y >> w >> h >> railtype >> axis)) return false;
		if (w <= 0 || h <= 0 || w > UINT16_MAX || h > UINT16_MAX || railtype < RAILTYPE_BEGIN || railtype >= RAILTYPE_END || axis < AXIS_X || axis > AXIS_Y) return false;
		clipboard.rail_station_areas.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(w), static_cast<uint16_t>(h),
				static_cast<RailType>(railtype), static_cast<Axis>(axis)});
	}

	if (!read_section("STATIONS", count)) return false;
	for (size_t i = 0; i < count; i++) {
		int x, y, railtype, axis;
		if (!(is >> x >> y >> railtype >> axis)) return false;
		if (railtype < RAILTYPE_BEGIN || railtype >= RAILTYPE_END || axis < AXIS_X || axis > AXIS_Y) return false;
		clipboard.rail_stations.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<RailType>(railtype), static_cast<Axis>(axis)});
	}

	if (clipboard.IsEmpty()) return false;

	_viewport_clipboard = std::move(clipboard);
	_viewport_clipboard_rotation = 0;
	return true;
}

static TileIndex AddMapOffset(TileIndex origin, int16_t x, int16_t y, uint16_t w = 1, uint16_t h = 1)
{
	uint target_x = TileX(origin) + x;
	uint target_y = TileY(origin) + y;
	if (target_x >= Map::SizeX() || target_y >= Map::SizeY()) return INVALID_TILE;
	if (target_x + w > Map::SizeX() || target_y + h > Map::SizeY()) return INVALID_TILE;
	return TileXY(target_x, target_y);
}

static TileIndex AddClipboardOffset(TileIndex origin, int16_t x, int16_t y, uint16_t w = 1, uint16_t h = 1)
{
	RotateClipboardArea(x, y, w, h);
	return AddMapOffset(origin, x, y, w, h);
}

static void CopyViewportInfrastructure(TileIndex start_tile, TileIndex end_tile)
{
	TileArea area(start_tile, end_tile);
	ViewportClipboard clipboard;
	std::vector<ViewportClipboardStationGroup> station_groups;
	clipboard.w = area.w;
	clipboard.h = area.h;

	for (TileIndex tile : area) {
		if (!IsValidTile(tile)) continue;

		int16_t x = static_cast<int16_t>(TileX(tile) - TileX(area.tile));
		int16_t y = static_cast<int16_t>(TileY(tile) - TileY(area.tile));

		if (IsPlainRailTile(tile)) {
			clipboard.rails.push_back({x, y, GetRailType(tile), GetTrackBits(tile)});
			if (HasSignals(tile)) {
				for (Track track = TRACK_BEGIN; track != TRACK_END; track++) {
					if (!HasTrack(tile, track) || !HasSignalOnTrack(tile, track)) continue;
					clipboard.signals.push_back({x, y, track, GetSignalType(tile, track), GetSignalVariant(tile, track),
							static_cast<uint8_t>(GetPresentSignals(tile) & SignalOnTrack(track))});
				}
			}
		} else if (HasStationTileRail(tile)) {
			StationID station_id = GetStationIndex(tile);
			RailType railtype = GetRailType(tile);
			Axis axis = GetRailStationAxis(tile);

			auto group = std::ranges::find_if(station_groups, [&](const ViewportClipboardStationGroup &group) {
				return group.station_id == station_id && group.railtype == railtype && group.axis == axis;
			});

			if (group == station_groups.end()) {
				station_groups.push_back({station_id, railtype, axis, TileArea(tile, 1, 1), {tile}});
			} else {
				group->area.Add(tile);
				group->tiles.push_back(tile);
			}
		}
	}

	for (const ViewportClipboardStationGroup &group : station_groups) {
		if (group.tiles.size() == static_cast<size_t>(group.area.w) * group.area.h) {
			clipboard.rail_station_areas.push_back({
				static_cast<int16_t>(TileX(group.area.tile) - TileX(area.tile)),
				static_cast<int16_t>(TileY(group.area.tile) - TileY(area.tile)),
				group.area.w,
				group.area.h,
				group.railtype,
				group.axis,
			});
		} else {
			for (TileIndex tile : group.tiles) {
				clipboard.rail_stations.push_back({
					static_cast<int16_t>(TileX(tile) - TileX(area.tile)),
					static_cast<int16_t>(TileY(tile) - TileY(area.tile)),
					group.railtype,
					group.axis,
				});
			}
		}
	}

	_viewport_clipboard = std::move(clipboard);
	_viewport_clipboard_rotation = 0;
}

static void PasteViewportInfrastructure(TileIndex origin)
{
	for (const ViewportClipboardRailStationArea &station : _viewport_clipboard.rail_station_areas) {
		int16_t x = station.x;
		int16_t y = station.y;
		uint16_t w = station.w;
		uint16_t h = station.h;
		RotateClipboardArea(x, y, w, h);
		TileIndex tile = AddMapOffset(origin, x, y, w, h);
		if (tile == INVALID_TILE) continue;

		Axis axis = RotateClipboardAxis(station.axis);
		uint8_t numtracks = static_cast<uint8_t>(axis == AXIS_X ? h : w);
		uint8_t plat_len = static_cast<uint8_t>(axis == AXIS_X ? w : h);
		Command<Commands::BuildRailStation>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_STATION, tile, station.railtype,
				axis, numtracks, plat_len, STAT_CLASS_DFLT, 0, StationID::Invalid(), false);
	}

	for (const ViewportClipboardRailStation &station : _viewport_clipboard.rail_stations) {
		TileIndex tile = AddClipboardOffset(origin, station.x, station.y);
		if (tile == INVALID_TILE) continue;

		Axis axis = RotateClipboardAxis(station.axis);
		Command<Commands::BuildRailStation>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_STATION, tile, station.railtype,
				axis, 1, 1, STAT_CLASS_DFLT, 0, StationID::Invalid(), false);
	}

	for (const ViewportClipboardRail &rail : _viewport_clipboard.rails) {
		TileIndex tile = AddClipboardOffset(origin, rail.x, rail.y);
		if (tile == INVALID_TILE) continue;

		TrackBits tracks = RotateClipboardTrackBits(rail.tracks);
		for (Track track = TRACK_BEGIN; track != TRACK_END; track++) {
			if (!HasTrack(tracks, track)) continue;
			Command<Commands::BuildRail>::Post(STR_ERROR_CAN_T_BUILD_RAILROAD_TRACK, tile, rail.railtype,
					track, _settings_client.gui.auto_remove_signals);
		}
	}

	for (const ViewportClipboardSignal &signal : _viewport_clipboard.signals) {
		TileIndex tile = AddClipboardOffset(origin, signal.x, signal.y);
		if (tile == INVALID_TILE) continue;

		Track track = RotateClipboardTrack(signal.track);
		uint8_t signals = RotateClipboardSignalBits(signal.track, signal.signals);
		Command<Commands::BuildSignal>::Post(STR_ERROR_CAN_T_BUILD_SIGNALS_HERE, CcPlaySound_CONSTRUCTION_RAIL,
				tile, track, signal.sigtype, signal.sigvar, false, false, false, SIGTYPE_BLOCK, SIGTYPE_LAST, 0, signals);
	}
}

static void UpdateViewportInfrastructurePastePreview()
{
	Dimension size = GetRotatedClipboardSize();
	SetTileSelectSize(size.width, size.height);
}

static void EqualizeViewportInfrastructurePastePreview(bool highest)
{
	if (_viewport_clipboard_mode != ViewportClipboardMode::Paste) return;
	if ((_thd.drawstyle & HT_DRAG_MASK) != HT_RECT) return;
	if (_thd.size.x <= 0 || _thd.size.y <= 0) return;

	struct PastePreviewCorner {
		TileIndex tile;
		TileIndex opposite_tile;
		uint height;
	};

	const int left = _thd.pos.x;
	const int top = _thd.pos.y;
	const int right = _thd.pos.x + _thd.size.x - TILE_SIZE;
	const int bottom = _thd.pos.y + _thd.size.y - TILE_SIZE;

	std::array<PastePreviewCorner, 4> corners = {{
		{TileVirtXY(left, top), TileVirtXY(right, bottom), 0},
		{TileVirtXY(right, top), TileVirtXY(left, bottom), 0},
		{TileVirtXY(left, bottom), TileVirtXY(right, top), 0},
		{TileVirtXY(right, bottom), TileVirtXY(left, top), 0},
	}};

	for (PastePreviewCorner &corner : corners) {
		corner.height = TileHeight(corner.tile);
	}

	const PastePreviewCorner *selected = &corners.front();
	for (const PastePreviewCorner &corner : corners) {
		if (highest ? corner.height > selected->height : corner.height < selected->height) selected = &corner;
	}

	Command<Commands::LevelLand>::Post(STR_ERROR_CAN_T_LEVEL_LAND_HERE, CcTerraform,
			selected->opposite_tile, selected->tile, _thd.diagonal, LM_LEVEL_FLAT);
}

static void RotateViewportInfrastructurePaste(bool clockwise)
{
	if (_viewport_clipboard_mode != ViewportClipboardMode::Paste) return;

	_viewport_clipboard_rotation = (_viewport_clipboard_rotation + (clockwise ? 1 : 3)) % 4;
	UpdateViewportInfrastructurePastePreview();
	MarkWholeScreenDirty();
}

static void BeginViewportInfrastructureCopy()
{
	SetObjectToPlace(SPR_CURSOR_MOUSE, PAL_NONE, HT_RECT, WC_MAIN_WINDOW, 0);
	_viewport_clipboard_mode = ViewportClipboardMode::Copy;
}

static void BeginViewportInfrastructurePaste()
{
	if (_viewport_clipboard.IsEmpty()) return;

	SetObjectToPlace(SPR_CURSOR_RAIL_STATION, PAL_NONE, HT_RECT, WC_MAIN_WINDOW, 0);
	_viewport_clipboard_mode = ViewportClipboardMode::Paste;
	UpdateViewportInfrastructurePastePreview();
}

static bool StoreViewportInfrastructureBlueprintFromCopySelection(uint slot)
{
	if (_viewport_clipboard_mode != ViewportClipboardMode::Copy || _thd.selend.x == -1) return false;

	CopyViewportInfrastructure(TileVirtXY(_thd.selstart.x, _thd.selstart.y), TileVirtXY(_thd.selend.x, _thd.selend.y));
	return SaveViewportInfrastructureBlueprint(slot);
}

static bool LoadViewportInfrastructureBlueprintForPaste(uint slot)
{
	if (!LoadViewportInfrastructureBlueprint(slot)) return false;

	BeginViewportInfrastructurePaste();
	return true;
}

/**
 * This code is shared for the majority of the pushbuttons.
 * Handles e.g. the pressing of a button (to build things), playing of click sound and sets certain parameters
 *
 * @param w Window which called the function
 * @param widget ID of the widget (=button) that called this function
 * @param cursor How should the cursor image change? E.g. cursor with depot image in it
 * @param mode Tile highlighting mode, e.g. drawing a rectangle or a dot on the ground
 * @return true if the button is clicked, false if it's unclicked
 */
bool HandlePlacePushButton(Window *w, WidgetID widget, CursorID cursor, HighLightStyle mode)
{
	if (w->IsWidgetDisabled(widget)) return false;

	SndClickBeep();
	w->SetDirty();

	if (w->IsWidgetLowered(widget)) {
		ResetObjectToPlace();
		return false;
	}

	SetObjectToPlace(cursor, PAL_NONE, mode, w->window_class, w->window_number);
	w->LowerWidget(widget);
	return true;
}


void CcPlaySound_EXPLOSION(Commands, const CommandCost &result, TileIndex tile)
{
	if (result.Succeeded() && _settings_client.sound.confirm) SndPlayTileFx(SND_12_EXPLOSION, tile);
}

/**
 * Zooms a viewport in a window in or out.
 * @param how Zooming direction.
 * @param w   Window owning the viewport.
 * @return Returns \c true if zooming step could be done, \c false if further zooming is not possible.
 * @note No button handling or what so ever is done.
 */
bool DoZoomInOutWindow(ZoomStateChange how, Window *w)
{
	assert(w != nullptr);

	switch (how) {
		case ZOOM_NONE:
			/* On initialisation of the viewport we don't do anything. */
			break;

		case ZOOM_IN: {
			ViewportData &vp = *w->viewport;
			if (vp.zoom <= _settings_client.gui.zoom_min) return false;
			--vp.zoom;
			vp.virtual_width >>= 1;
			vp.virtual_height >>= 1;

			vp.scrollpos_x += vp.virtual_width >> 1;
			vp.scrollpos_y += vp.virtual_height >> 1;
			vp.dest_scrollpos_x = vp.scrollpos_x;
			vp.dest_scrollpos_y = vp.scrollpos_y;
			break;
		}

		case ZOOM_OUT: {
			ViewportData &vp = *w->viewport;
			if (vp.zoom >= _settings_client.gui.zoom_max) return false;
			++vp.zoom;

			vp.scrollpos_x -= vp.virtual_width >> 1;
			vp.scrollpos_y -= vp.virtual_height >> 1;
			vp.dest_scrollpos_x = vp.scrollpos_x;
			vp.dest_scrollpos_y = vp.scrollpos_y;

			vp.virtual_width <<= 1;
			vp.virtual_height <<= 1;
			break;
		}
	}

	if (w->viewport != nullptr) { // the viewport can be null when how == ZOOM_NONE
		w->viewport->virtual_left = w->viewport->scrollpos_x;
		w->viewport->virtual_top = w->viewport->scrollpos_y;
	}

	/* Update the windows that have zoom-buttons to perhaps disable their buttons */
	w->InvalidateData();
	return true;
}

void ZoomInOrOutToCursorWindow(bool in, Window *w)
{
	assert(w != nullptr);

	if (_game_mode != GM_MENU) {
		if ((in && w->viewport->zoom <= _settings_client.gui.zoom_min) || (!in && w->viewport->zoom >= _settings_client.gui.zoom_max)) return;

		Point pt = GetTileZoomCenterWindow(in, w);
		if (pt.x != -1) {
			ScrollWindowTo(pt.x, pt.y, -1, w, true);

			DoZoomInOutWindow(in ? ZOOM_IN : ZOOM_OUT, w);
		}
	}
}

void FixTitleGameZoom(int zoom_adjust)
{
	if (_game_mode != GM_MENU) return;

	Viewport &vp = *GetMainWindow()->viewport;

	/* Adjust the zoom in/out.
	 * Can't simply add, since operator+ is not defined on the ZoomLevel type. */
	vp.zoom = _gui_zoom;
	while (zoom_adjust < 0 && vp.zoom != _settings_client.gui.zoom_min) {
		vp.zoom--;
		zoom_adjust++;
	}
	while (zoom_adjust > 0 && vp.zoom != _settings_client.gui.zoom_max) {
		vp.zoom++;
		zoom_adjust--;
	}

	vp.virtual_width = ScaleByZoom(vp.width, vp.zoom);
	vp.virtual_height = ScaleByZoom(vp.height, vp.zoom);
}

static constexpr std::initializer_list<NWidgetPart> _nested_main_window_widgets = {
	NWidget(NWID_VIEWPORT, Colours::Invalid, WID_M_VIEWPORT), SetResize(1, 1),
};

enum GlobalHotKeys : int32_t {
	GHK_QUIT,
	GHK_ABANDON,
	GHK_CONSOLE,
	GHK_BOUNDING_BOXES,
	GHK_DIRTY_BLOCKS,
	GHK_WIDGET_OUTLINES,
	GHK_CENTER,
	GHK_CENTER_ZOOM,
	GHK_RESET_OBJECT_TO_PLACE,
	GHK_DELETE_WINDOWS,
	GHK_DELETE_NONVITAL_WINDOWS,
	GHK_DELETE_ALL_MESSAGES,
	GHK_REFRESH_SCREEN,
	GHK_CRASH,
	GHK_MONEY,
	GHK_UPDATE_COORDS,
	GHK_TOGGLE_TRANSPARENCY,
	GHK_TOGGLE_INVISIBILITY = GHK_TOGGLE_TRANSPARENCY + 9,
	GHK_TRANSPARENCY_TOOLBAR = GHK_TOGGLE_INVISIBILITY + 8,
	GHK_TRANSPARENCY,
	GHK_CHAT,
	GHK_CHAT_ALL,
	GHK_CHAT_COMPANY,
	GHK_CHAT_SERVER,
	GHK_CLOSE_NEWS,
	GHK_CLOSE_ERROR,
	GHK_COPY_VIEWPORT_INFRASTRUCTURE,
	GHK_PASTE_VIEWPORT_INFRASTRUCTURE,
	GHK_EQUALIZE_VIEWPORT_INFRASTRUCTURE_PASTE_LOWEST,
	GHK_EQUALIZE_VIEWPORT_INFRASTRUCTURE_PASTE_HIGHEST,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_1,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_2,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_3,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_4,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_5,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_6,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_7,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_8,
	GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_9,
};

struct MainWindow : Window
{
	MainWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(0);
		this->flags.Reset(WindowFlag::WhiteBorder);
		ResizeWindow(this, _screen.width, _screen.height);

		NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_M_VIEWPORT);
		nvp->InitializeViewport(this, TileXY(32, 32), ScaleZoomGUI(ZoomLevel::Viewport));

		this->viewport->overlay = std::make_shared<LinkGraphOverlay>(this, WID_M_VIEWPORT, CargoTypes{}, CompanyMask{}, 2);
		this->refresh_timeout.Reset();
	}

	/** Refresh the link-graph overlay. */
	void RefreshLinkGraph()
	{
		if (this->viewport->overlay->GetCargoMask().None() ||
				this->viewport->overlay->GetCompanyMask().None()) {
			return;
		}

		this->viewport->overlay->SetDirty();
		this->GetWidget<NWidgetBase>(WID_M_VIEWPORT)->SetDirty(this);
	}

	/** Refresh the link-graph overlay on a regular interval. */
	const IntervalTimer<TimerWindow> refresh_interval = {std::chrono::milliseconds(7650), [this](auto) {
		RefreshLinkGraph();
	}};

	/**
	 * Sometimes when something happened, force an update to the link-graph a bit sooner.
	 *
	 * We don't do it instantly on those changes, as for example when you are scrolling,
	 * constantly refreshing the link-graph would be very slow. So we delay it a bit,
	 * and only draw it once the scrolling settles down.
	 */
	TimeoutTimer<TimerWindow> refresh_timeout = {std::chrono::milliseconds(450), [this]() {
		RefreshLinkGraph();
	}};

	void OnPaint() override
	{
		this->DrawWidgets();
		if (_game_mode == GM_MENU) {
			static const std::initializer_list<SpriteID> title_sprites = {SPR_OTTD_O, SPR_OTTD_P, SPR_OTTD_E, SPR_OTTD_N, SPR_OTTD_T, SPR_OTTD_T, SPR_OTTD_D};
			uint letter_spacing = ScaleGUITrad(10);
			int name_width = static_cast<int>(std::size(title_sprites) - 1) * letter_spacing;

			for (const SpriteID &sprite : title_sprites) {
				name_width += GetSpriteSize(sprite).width;
			}
			int off_x = (this->width - name_width) / 2;

			for (const SpriteID &sprite : title_sprites) {
				DrawSprite(sprite, PAL_NONE, off_x, ScaleGUITrad(50));
				off_x += GetSpriteSize(sprite).width + letter_spacing;
			}

			int text_y = this->height - GetCharacterHeight(FontSize::Normal) * 2;
			DrawString(0, this->width - 1, text_y, STR_INTRO_VERSION, TC_WHITE, SA_CENTER);
		}
	}

	EventState OnHotkey(int hotkey) override
	{
		if (hotkey == GHK_QUIT) {
			HandleExitGameRequest();
			return ES_HANDLED;
		}

		/* Disable all key shortcuts, except quit shortcuts when
		 * generating the world, otherwise they create threading
		 * problem during the generating, resulting in random
		 * assertions that are hard to trigger and debug */
		if (HasModalProgress()) return ES_NOT_HANDLED;

		switch (hotkey) {
			case GHK_ABANDON:
				/* No point returning from the main menu to itself */
				if (_game_mode == GM_MENU) return ES_HANDLED;
				if (_settings_client.gui.autosave_on_exit) {
					DoExitSave();
					_switch_mode = SM_MENU;
				} else {
					AskExitToGameMenu();
				}
				return ES_HANDLED;

			case GHK_CONSOLE:
				IConsoleSwitch();
				return ES_HANDLED;

			case GHK_BOUNDING_BOXES:
				ToggleBoundingBoxes();
				return ES_HANDLED;

			case GHK_DIRTY_BLOCKS:
				ToggleDirtyBlocks();
				return ES_HANDLED;

			case GHK_WIDGET_OUTLINES:
				ToggleWidgetOutlines();
				return ES_HANDLED;
		}

		if (_game_mode == GM_MENU) return ES_NOT_HANDLED;

		switch (hotkey) {
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_1:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_2:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_3:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_4:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_5:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_6:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_7:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_8:
			case GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_9: {
				uint slot = static_cast<uint>(hotkey - GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_1 + 1);
				bool handled = _viewport_clipboard_mode == ViewportClipboardMode::Copy ?
						StoreViewportInfrastructureBlueprintFromCopySelection(slot) :
						LoadViewportInfrastructureBlueprintForPaste(slot);
				return handled ? ES_HANDLED : ES_NOT_HANDLED;
			}

			case GHK_CENTER:
			case GHK_CENTER_ZOOM: {
				Point pt = GetTileBelowCursor();
				if (pt.x != -1) {
					bool instant = (hotkey == GHK_CENTER_ZOOM && this->viewport->zoom != _settings_client.gui.zoom_min);
					if (hotkey == GHK_CENTER_ZOOM) MaxZoomInOut(ZOOM_IN, this);
					ScrollMainWindowTo(pt.x, pt.y, -1, instant);
				}
				break;
			}

			case GHK_RESET_OBJECT_TO_PLACE: ResetObjectToPlace(); break;
			case GHK_COPY_VIEWPORT_INFRASTRUCTURE: BeginViewportInfrastructureCopy(); break;
			case GHK_PASTE_VIEWPORT_INFRASTRUCTURE: BeginViewportInfrastructurePaste(); break;
			case GHK_EQUALIZE_VIEWPORT_INFRASTRUCTURE_PASTE_LOWEST:
				if (_viewport_clipboard_mode != ViewportClipboardMode::Paste) return ES_NOT_HANDLED;
				EqualizeViewportInfrastructurePastePreview(false);
				break;

			case GHK_EQUALIZE_VIEWPORT_INFRASTRUCTURE_PASTE_HIGHEST:
				if (_viewport_clipboard_mode != ViewportClipboardMode::Paste) return ES_NOT_HANDLED;
				EqualizeViewportInfrastructurePastePreview(true);
				break;

			case GHK_DELETE_WINDOWS: CloseNonVitalWindows(); break;
			case GHK_DELETE_NONVITAL_WINDOWS: CloseAllNonVitalWindows(); break;
			case GHK_DELETE_ALL_MESSAGES: DeleteAllMessages(); break;
			case GHK_REFRESH_SCREEN: MarkWholeScreenDirty(); break;

			case GHK_CRASH: // Crash the game
				*(volatile uint8_t *)nullptr = 0;
				break;

			case GHK_MONEY: // Gimme money
				/* You can only cheat for money in singleplayer mode. */
				if (!_networking) Command<Commands::MoneyCheat>::Post(10000000);
				break;

			case GHK_UPDATE_COORDS: // Update the coordinates of all station signs
				UpdateAllVirtCoords();
				break;

			case GHK_TOGGLE_TRANSPARENCY:
			case GHK_TOGGLE_TRANSPARENCY + 1:
			case GHK_TOGGLE_TRANSPARENCY + 2:
			case GHK_TOGGLE_TRANSPARENCY + 3:
			case GHK_TOGGLE_TRANSPARENCY + 4:
			case GHK_TOGGLE_TRANSPARENCY + 5:
			case GHK_TOGGLE_TRANSPARENCY + 6:
			case GHK_TOGGLE_TRANSPARENCY + 7:
			case GHK_TOGGLE_TRANSPARENCY + 8:
				/* Transparency toggle hot keys */
				ToggleTransparency((TransparencyOption)(hotkey - GHK_TOGGLE_TRANSPARENCY));
				MarkWholeScreenDirty();
				break;

			case GHK_TOGGLE_INVISIBILITY:
			case GHK_TOGGLE_INVISIBILITY + 1:
			case GHK_TOGGLE_INVISIBILITY + 2:
			case GHK_TOGGLE_INVISIBILITY + 3:
			case GHK_TOGGLE_INVISIBILITY + 4:
			case GHK_TOGGLE_INVISIBILITY + 5:
			case GHK_TOGGLE_INVISIBILITY + 6:
			case GHK_TOGGLE_INVISIBILITY + 7:
				/* Invisibility toggle hot keys */
				ToggleInvisibilityWithTransparency((TransparencyOption)(hotkey - GHK_TOGGLE_INVISIBILITY));
				MarkWholeScreenDirty();
				break;

			case GHK_TRANSPARENCY_TOOLBAR:
				ShowTransparencyToolbar();
				break;

			case GHK_TRANSPARENCY:
				ResetRestoreAllTransparency();
				break;

			case GHK_CHAT: // smart chat; send to team if any, otherwise to all
				if (_networking) {
					const NetworkClientInfo *cio = NetworkClientInfo::GetByClientID(_network_own_client_id);
					if (cio == nullptr) break;

					ShowNetworkChatQueryWindow(NetworkClientPreferTeamChat(cio) ? NetworkChatDestinationType::Team : NetworkChatDestinationType::Broadcast, cio->client_playas.base());
				}
				break;

			case GHK_CHAT_ALL: // send text message to all clients
				if (_networking) ShowNetworkChatQueryWindow(NetworkChatDestinationType::Broadcast, 0);
				break;

			case GHK_CHAT_COMPANY: // send text to all team mates
				if (_networking) {
					const NetworkClientInfo *cio = NetworkClientInfo::GetByClientID(_network_own_client_id);
					if (cio == nullptr) break;

					ShowNetworkChatQueryWindow(NetworkChatDestinationType::Team, cio->client_playas.base());
				}
				break;

			case GHK_CHAT_SERVER: // send text to the server
				if (_networking && !_network_server) {
					ShowNetworkChatQueryWindow(NetworkChatDestinationType::Client, CLIENT_ID_SERVER);
				}
				break;

			case GHK_CLOSE_NEWS: // close active news window
				if (!HideActiveNewsMessage()) return ES_NOT_HANDLED;
				break;

			case GHK_CLOSE_ERROR: // close active error window
				if (!HideActiveErrorMessage()) return ES_NOT_HANDLED;
				break;

			default: return ES_NOT_HANDLED;
		}
		return ES_HANDLED;
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (_viewport_clipboard_mode) {
			case ViewportClipboardMode::Copy:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_DEMOLISH_AREA);
				break;

			case ViewportClipboardMode::Paste:
				PasteViewportInfrastructure(tile);
				UpdateViewportInfrastructurePastePreview();
				break;

			case ViewportClipboardMode::None:
				break;
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		if (_viewport_clipboard_mode != ViewportClipboardMode::Copy) return;
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (_viewport_clipboard_mode != ViewportClipboardMode::Copy || pt.x == -1) return;

		CopyViewportInfrastructure(start_tile, end_tile);
		ResetObjectToPlace();
	}

	void OnPlaceObjectAbort() override
	{
		_viewport_clipboard_mode = ViewportClipboardMode::None;
	}

	bool OnRightClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		if (widget != WID_M_VIEWPORT || _viewport_clipboard_mode != ViewportClipboardMode::Paste) return false;
		RotateViewportInfrastructurePaste(true);
		return true;
	}

	bool OnMiddleClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		if (widget != WID_M_VIEWPORT || _viewport_clipboard_mode != ViewportClipboardMode::Paste) return false;
		RotateViewportInfrastructurePaste(false);
		return true;
	}

	void OnScroll(Point delta) override
	{
		this->viewport->scrollpos_x += ScaleByZoom(delta.x, this->viewport->zoom);
		this->viewport->scrollpos_y += ScaleByZoom(delta.y, this->viewport->zoom);
		this->viewport->dest_scrollpos_x = this->viewport->scrollpos_x;
		this->viewport->dest_scrollpos_y = this->viewport->scrollpos_y;
		this->refresh_timeout.Reset();
	}

	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_M_VIEWPORT) return;
		if (_settings_client.gui.scrollwheel_scrolling != ScrollWheelScrolling::Off) {
			bool in = wheel < 0;

			/* When following, only change zoom - otherwise zoom to the cursor. */
			if (this->viewport->follow_vehicle != VehicleID::Invalid()) {
				DoZoomInOutWindow(in ? ZOOM_IN : ZOOM_OUT, this);
			} else {
				ZoomInOrOutToCursorWindow(in, this);
			}
		}
	}

	void OnResize() override
	{
		if (this->viewport != nullptr) {
			NWidgetViewport *nvp = this->GetWidget<NWidgetViewport>(WID_M_VIEWPORT);
			nvp->UpdateViewportCoordinates(this);
			this->refresh_timeout.Reset();
		}
	}

	bool OnTooltip([[maybe_unused]] Point pt, WidgetID widget, TooltipCloseCondition close_cond) override
	{
		if (widget != WID_M_VIEWPORT) return false;
		return this->viewport->overlay->ShowTooltip(pt, close_cond);
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		/* Forward the message to the appropriate toolbar (ingame or scenario editor) */
		InvalidateWindowData(WC_MAIN_TOOLBAR, 0, data, true);
	}

	static inline HotkeyList hotkeys{"global", {
		Hotkey({'Q' | WKC_CTRL, 'Q' | WKC_META}, "quit", GHK_QUIT),
		Hotkey({'W' | WKC_CTRL, 'W' | WKC_META}, "abandon", GHK_ABANDON),
		Hotkey(WKC_BACKQUOTE, "console", GHK_CONSOLE),
		Hotkey('B' | WKC_CTRL, "bounding_boxes", GHK_BOUNDING_BOXES),
		Hotkey('I' | WKC_CTRL, "dirty_blocks", GHK_DIRTY_BLOCKS),
		Hotkey('O' | WKC_CTRL, "widget_outlines", GHK_WIDGET_OUTLINES),
		Hotkey('C', "center", GHK_CENTER),
		Hotkey('Z', "center_zoom", GHK_CENTER_ZOOM),
		Hotkey(WKC_ESC, "reset_object_to_place", GHK_RESET_OBJECT_TO_PLACE),
		Hotkey(WKC_DELETE, "delete_windows", GHK_DELETE_WINDOWS),
		Hotkey(WKC_DELETE | WKC_SHIFT, "delete_all_windows", GHK_DELETE_NONVITAL_WINDOWS),
		Hotkey(WKC_DELETE | WKC_CTRL, "delete_all_messages", GHK_DELETE_ALL_MESSAGES),
		Hotkey('R' | WKC_CTRL, "refresh_screen", GHK_REFRESH_SCREEN),
		Hotkey('1' | WKC_ALT, "viewport_infrastructure_blueprint_1", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_1),
		Hotkey('2' | WKC_ALT, "viewport_infrastructure_blueprint_2", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_2),
		Hotkey('3' | WKC_ALT, "viewport_infrastructure_blueprint_3", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_3),
		Hotkey('4' | WKC_ALT, "viewport_infrastructure_blueprint_4", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_4),
		Hotkey('5' | WKC_ALT, "viewport_infrastructure_blueprint_5", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_5),
		Hotkey('6' | WKC_ALT, "viewport_infrastructure_blueprint_6", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_6),
		Hotkey('7' | WKC_ALT, "viewport_infrastructure_blueprint_7", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_7),
		Hotkey('8' | WKC_ALT, "viewport_infrastructure_blueprint_8", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_8),
		Hotkey('9' | WKC_ALT, "viewport_infrastructure_blueprint_9", GHK_VIEWPORT_INFRASTRUCTURE_BLUEPRINT_9),
#if defined(_DEBUG)
		Hotkey('0' | WKC_ALT, "crash_game", GHK_CRASH),
		Hotkey('1' | WKC_ALT, "money", GHK_MONEY),
		Hotkey('2' | WKC_ALT, "update_coordinates", GHK_UPDATE_COORDS),
#endif
		Hotkey('1' | WKC_CTRL, "transparency_signs", GHK_TOGGLE_TRANSPARENCY),
		Hotkey('2' | WKC_CTRL, "transparency_trees", GHK_TOGGLE_TRANSPARENCY + 1),
		Hotkey('3' | WKC_CTRL, "transparency_houses", GHK_TOGGLE_TRANSPARENCY + 2),
		Hotkey('4' | WKC_CTRL, "transparency_industries", GHK_TOGGLE_TRANSPARENCY + 3),
		Hotkey('5' | WKC_CTRL, "transparency_buildings", GHK_TOGGLE_TRANSPARENCY + 4),
		Hotkey('6' | WKC_CTRL, "transparency_bridges", GHK_TOGGLE_TRANSPARENCY + 5),
		Hotkey('7' | WKC_CTRL, "transparency_structures", GHK_TOGGLE_TRANSPARENCY + 6),
		Hotkey('8' | WKC_CTRL, "transparency_catenary", GHK_TOGGLE_TRANSPARENCY + 7),
		Hotkey('9' | WKC_CTRL, "transparency_loading", GHK_TOGGLE_TRANSPARENCY + 8),
		Hotkey('1' | WKC_CTRL | WKC_SHIFT, "invisibility_signs", GHK_TOGGLE_INVISIBILITY),
		Hotkey('2' | WKC_CTRL | WKC_SHIFT, "invisibility_trees", GHK_TOGGLE_INVISIBILITY + 1),
		Hotkey('3' | WKC_CTRL | WKC_SHIFT, "invisibility_houses", GHK_TOGGLE_INVISIBILITY + 2),
		Hotkey('4' | WKC_CTRL | WKC_SHIFT, "invisibility_industries", GHK_TOGGLE_INVISIBILITY + 3),
		Hotkey('5' | WKC_CTRL | WKC_SHIFT, "invisibility_buildings", GHK_TOGGLE_INVISIBILITY + 4),
		Hotkey('6' | WKC_CTRL | WKC_SHIFT, "invisibility_bridges", GHK_TOGGLE_INVISIBILITY + 5),
		Hotkey('7' | WKC_CTRL | WKC_SHIFT, "invisibility_structures", GHK_TOGGLE_INVISIBILITY + 6),
		Hotkey('8' | WKC_CTRL | WKC_SHIFT, "invisibility_catenary", GHK_TOGGLE_INVISIBILITY + 7),
		Hotkey('X' | WKC_CTRL, "transparency_toolbar", GHK_TRANSPARENCY_TOOLBAR),
		Hotkey('X', "toggle_transparency", GHK_TRANSPARENCY),
		Hotkey({WKC_RETURN, 'T'}, "chat", GHK_CHAT),
		Hotkey({WKC_SHIFT | WKC_RETURN, WKC_SHIFT | 'T'}, "chat_all", GHK_CHAT_ALL),
		Hotkey({WKC_CTRL | WKC_RETURN, WKC_CTRL | 'T'}, "chat_company", GHK_CHAT_COMPANY),
		Hotkey({WKC_CTRL | WKC_SHIFT | WKC_RETURN, WKC_CTRL | WKC_SHIFT | 'T'}, "chat_server", GHK_CHAT_SERVER),
		Hotkey(WKC_SPACE, "close_news", GHK_CLOSE_NEWS),
		Hotkey(WKC_SPACE, "close_error", GHK_CLOSE_ERROR),
		Hotkey('C' | WKC_CTRL, "copy_viewport_infrastructure", GHK_COPY_VIEWPORT_INFRASTRUCTURE),
		Hotkey('V' | WKC_CTRL, "paste_viewport_infrastructure", GHK_PASTE_VIEWPORT_INFRASTRUCTURE),
		Hotkey('F', "equalize_viewport_infrastructure_paste_lowest", GHK_EQUALIZE_VIEWPORT_INFRASTRUCTURE_PASTE_LOWEST),
		Hotkey('G', "equalize_viewport_infrastructure_paste_highest", GHK_EQUALIZE_VIEWPORT_INFRASTRUCTURE_PASTE_HIGHEST),
	}};
};

/** Window definition for the main window. */
static WindowDesc _main_window_desc(
	WindowPosition::Manual, {}, 0, 0,
	WC_MAIN_WINDOW, WC_NONE,
	WindowDefaultFlag::NoClose,
	_nested_main_window_widgets,
	&MainWindow::hotkeys
);

/**
 * Does the given keycode match one of the keycodes bound to 'quit game'?
 * @param keycode The keycode that was pressed by the user.
 * @return True iff the keycode matches one of the hotkeys for 'quit'.
 */
bool IsQuitKey(uint16_t keycode)
{
	int num = MainWindow::hotkeys.CheckMatch(keycode);
	return num == GHK_QUIT;
}


void ShowSelectGameWindow();

/**
 * Initialise the default colours (remaps and the likes), and load the main windows.
 */
void SetupColoursAndInitialWindow()
{
	for (Colours i = Colours::Begin; i != Colours::End; i++) {
		const uint8_t *b = GetNonSprite(GetColourPalette(i), SpriteType::Recolour) + 1;
		assert(b != nullptr);
		for (ColourShade j = SHADE_BEGIN; j < SHADE_END; j++) {
			SetColourGradient(i, j, PixelColour{b[0xC6 + j]});
		}
	}

	new MainWindow(_main_window_desc);

	/* XXX: these are not done */
	switch (_game_mode) {
		default: NOT_REACHED();
		case GM_MENU:
			ShowSelectGameWindow();
			break;

		case GM_NORMAL:
		case GM_EDITOR:
			ShowVitalWindows();
			break;
	}
}

/**
 * Show the vital in-game windows.
 */
void ShowVitalWindows()
{
	AllocateToolbar();

	/* Status bad only for normal games */
	if (_game_mode == GM_EDITOR) return;

	ShowStatusBar();
}

/**
 * Size of the application screen changed.
 * Adapt the game screen-size, re-allocate the open windows, and repaint everything
 */
void GameSizeChanged()
{
	_cur_resolution.width  = _screen.width;
	_cur_resolution.height = _screen.height;
	ScreenSizeChanged();
	RelocateAllWindows(_screen.width, _screen.height);
	MarkWholeScreenDirty();
}
