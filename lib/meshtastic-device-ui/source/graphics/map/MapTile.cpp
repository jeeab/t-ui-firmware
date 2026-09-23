#include "graphics/map/MapTile.h"
#include "graphics/map/MapTileSettings.h"
#include "graphics/map/TileService.h"
#include "lvgl.h"
#include "util/ILog.h"

#include <assert.h>

LV_IMAGE_DECLARE(img_no_tile_image);

OSMTiles<lv_obj_t> *osm = nullptr;

MapTile::MapTile(uint32_t xTile, uint32_t yTile)
    : OSMTiles<lv_obj_t>::Tile(xTile, yTile, MapTileSettings::getZoomLevel()), img(nullptr), lbl(nullptr)
{
    // singleton should be already created
    assert(osm != nullptr);
}

/**
 * load map tile to display position x/y
 */
bool MapTile::load(lv_obj_t *p, int16_t posx, int16_t posy, const lv_image_dsc_t *img_src)
{
    x = posx;
    y = posy;
    ozdx = ozdy = 0; // a fresh load is not overzoomed until the block below says so
    if (!p)
        return false;
    removeImage();
    img = lv_image_create(p);
    lv_obj_set_pos(img, posx, posy);
    lv_obj_set_style_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(img, MapTileSettings::getTileSize(), MapTileSettings::getTileSize());
    if (MapTileSettings::getDebug()) {
        lv_obj_set_style_border_width(img, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lbl = lv_label_create(img);
        lv_obj_set_pos(lbl, 0, 0);
        lv_obj_set_size(lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xff101010), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text_fmt(lbl, "(%d/%d/%d) -> %d,%d", MapTileSettings::getZoomLevel(), xTile, yTile, posx, posy);
    }

    bool result = false;
#if LV_USE_FS_ARDUINO_SD
    // use lvgl built-in img loader
    char fname[128];
    fname[0] = LV_FS_ARDUINO_SD_LETTER;
    sprintf(&fname[1], ":%s/%s%d/%d/%d.%s", MapTileSettings::getPrefix(), MapTileSettings::getTileStyle(), zoomLevel, xTile,
            yTile, MapTileSettings::getTileFormat());
    ILOG_DEBUG("SD file: %s", fname);
    lv_image_set_src(img, fname);
    if (lv_image_get_src((lv_obj_t *)img)) {
        result = true;
    }
#endif
    // use configured TileService
    if (!result) {
        result = osm->load(*this, img);
    }

    // ⭐ OVERZOOM. Jake: "when you have no more map tiles at that zone. Could it like zoom in on
    // the actual map tile from the previous? It would be blurry but at least a bit better than
    // nothing right?" - yes, and it is what every map renderer does. USGS stops around z16 and
    // past that this drew a grey placeholder, which is worse than a blurry map: blurry still
    // shows the shape of the valley and where the track runs.
    //
    // ⛔ THIS MUST GO THROUGH osm->load(), NOT the lv_fs path above it. LV_USE_FS_ARDUINO_SD is
    // 0 in our lv_conf.h, so that block is dead code - the first version of this feature copied
    // its #if and was therefore never compiled in at all. Jake: "the extra zoom thing on the
    // maps isnt working. it just shows the 'no tile' placeholder still."
    //
    // Tile (z,x,y) lives inside (z-up, x>>up, y>>up). Rather than offset the source - which is
    // not what lv_image_set_offset_* does; it moves an unscaled rect in WIDGET pixels, before
    // lv_draw_image applies the scale - grow the object to the whole magnified parent and slide
    // it so our quadrant lands on our slot. pivot (0,0) keeps source (0,0) at the object's
    // top-left so it magnifies right and down.
    //
    // ⭐ THE OVERLAP IS HARMLESS, and that is the trick. This object now covers its siblings'
    // slots, but every child of the same parent computes (pos - sub*tileSize) from a pos that
    // differs by exactly tileSize per unit of sub, so they all land on the SAME rect and draw
    // the SAME magnified parent. Siblings paint identical pixels. No clipping container needed.
    if (!result) {
        const int tileSize = MapTileSettings::getTileSize();
        constexpr int kMaxOverzoom = 3; // 2x, 4x, 8x - past that it tells you nothing
        for (int up = 1; up <= kMaxOverzoom && !result; up++) {
            if ((int)zoomLevel - up < 0)
                break;
            OSMTiles<lv_obj_t>::Tile parent((uint32_t)(xTile >> up), (uint32_t)(yTile >> up), (uint8_t)(zoomLevel - up));
            if (!osm->load(parent, img))
                continue; // that ancestor is missing too - try one further up
            const int factor = 1 << up;
            const int subX = (int)(xTile & (uint32_t)(factor - 1));
            const int subY = (int)(yTile & (uint32_t)(factor - 1));
            ozdx = (int16_t)(-subX * tileSize);
            ozdy = (int16_t)(-subY * tileSize);
            lv_obj_set_size(img, tileSize * factor, tileSize * factor);
            lv_obj_set_pos(img, x + ozdx, y + ozdy);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TOP_LEFT);
            lv_image_set_pivot(img, 0, 0);
            lv_image_set_scale(img, LV_SCALE_NONE * factor);
            lv_image_set_antialias(img, false); // a blown-up tile: nearest-neighbour is honest and cheap
            lv_obj_set_style_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            ILOG_DEBUG("overzoom %d/%d/%d from parent %d/%d/%d (%dx)", zoomLevel, xTile, yTile, (int)zoomLevel - up,
                       xTile >> up, yTile >> up, factor);
            result = true;
        }
    }

    if (!result) {
        {
            if (img_src) {
                // ILOG_DEBUG("set no-tile-image (%d/%d/%d)", MapTileSettings::getZoomLevel(), xTile, yTile);
                lv_image_set_src((lv_obj_t *)img, img_src);
                lv_obj_set_style_opa(img, 100, LV_PART_MAIN | LV_STATE_DEFAULT);
                if (!MapTileSettings::getDebug()) {
                    lv_obj_t *lbl = lv_label_create(img);
                    lv_obj_set_pos(lbl, 0, 50);
                    lv_obj_set_align(lbl, LV_ALIGN_CENTER);
                    lv_obj_set_size(lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_text_color(lbl, lv_color_hex(0xff505050), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_fmt(lbl, "(%d/%d/%d)", MapTileSettings::getZoomLevel(), xTile, yTile);
                }
            }
        }
    }
    return result;
}

bool MapTile::move(int16_t posx, int16_t posy)
{
    x += posx;
    y += posy;
    // ozdx/ozdy are 0 for a normal tile. For an overzoomed one they are where the magnified
    // parent has to sit for OUR quadrant to land on OUR slot - drop them and panning tears the
    // alignment apart one step at a time.
    if (img)
        lv_obj_set_pos(img, x + ozdx, y + ozdy);
    if (MapTileSettings::getDebug()) {
        lv_label_set_text_fmt(lbl, "(%d/%d/%d) -> %d,%d", MapTileSettings::getZoomLevel(), xTile, yTile, x, y);
    }
    return true;
}

void MapTile::removeImage(void)
{
    if (!img) {
        return;
    }

    const void *src = lv_image_get_src(img);
    /* clear the source first so LVGL stops referencing it */
    lv_image_set_src(img, NULL);

    if (src && lv_image_src_get_type(src) == LV_IMAGE_SRC_VARIABLE) {
        const lv_image_dsc_t *img_dsc = (const lv_image_dsc_t *)src;
        const bool ownedByMapTile =
            (img_dsc->header.magic == LV_IMAGE_HEADER_MAGIC) && (img_dsc->header.flags & LV_IMAGE_FLAGS_USER1);
        if (ownedByMapTile) {
            // ILOG_INFO("%d/%d: free tile image %d bytes", xTile, yTile, img_dsc->data_size);
            if (img_dsc->data) {
                lv_free((void *)img_dsc->data);
            }
            lv_free((void *)img_dsc);
        } else {
            // ILOG_INFO("%d/%d: tile image %d bytes -> not owned", xTile, yTile, img_dsc->data_size);
        }
    }

    lv_obj_delete(img);
    img = nullptr;
}

MapTile::~MapTile()
{
    // ILOG_DEBUG("MapTile::~MapTile %d/%d/%d", zoomLevel, xTile, yTile);
    removeImage();
}
