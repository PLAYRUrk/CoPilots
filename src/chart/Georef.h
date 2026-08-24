#pragma once
#include "ChartTypes.h"

#include <cmath>

// ---------------------------------------------------------------------------
// ChartFox georeference math — lat/lon → canonical chart pixel.
//
// A georeference maps the chart PAGE onto Web Mercator, not the other way
// round: (tx, ty) is a point in EPSG:3857 METRES, k is metres per unit of
// normalised page coordinate, and transform_angle is the rotation in RADIANS
// (values around ±1.5708 show up on charts whose artwork is turned 90° on the
// page).  Page coordinates are normalised by the page HEIGHT, measured in the
// orientation the page is displayed in — which is exactly what PDFium reports
// and rasterises, so pdf_page_rotation needs no handling of our own.
//
//     merc = (tx, ty) + k · R(angle) · (a, b)        [page → world]
//
// The viewer needs the inverse:
//
//     d = merc(lat, lon) − (tx, ty)
//     a = ( d.x·cos + d.y·sin) / k
//     b = (−d.x·sin + d.y·cos) / k
//     x =  a · pageHeightPx        y = −b · pageHeightPx
//
// Verified against live ChartFox data by rendering the PDFs and overlaying
// known positions: KJFK AIRPORT DIAGRAM (portrait page, angle ≈ −π/2) lands on
// the printed lat/lon graticule, UUEE AERODROME CHART (page rotated 270°) lands
// on all six published runway thresholds.
// ---------------------------------------------------------------------------

namespace cp {
namespace chart {

constexpr double kPi     = 3.14159265358979323846;
constexpr double kEarthR = 6378137.0;             // EPSG:3857 sphere

// WGS84 → Web Mercator metres.
inline void latLonToMercator(double latDeg, double lonDeg, double& mx, double& my)
{
    double lat = latDeg;
    if (lat >  85.05112878) lat =  85.05112878;   // Mercator singularity clamp
    if (lat < -85.05112878) lat = -85.05112878;
    mx = kEarthR * lonDeg * kPi / 180.0;
    my = kEarthR * std::log(std::tan(kPi / 4.0 + lat * kPi / 360.0));
}

// Normalised page coordinates (a right, b up, origin at the page's top-left).
inline void latLonToPageNorm(const ChartGeoref& g, double latDeg, double lonDeg,
                             double& a, double& b)
{
    double mx, my;
    latLonToMercator(latDeg, lonDeg, mx, my);
    const double dx = mx - g.tx, dy = my - g.ty;
    const double c = std::cos(g.transformAngle), s = std::sin(g.transformAngle);
    a = ( dx * c + dy * s) / g.k;
    b = (-dx * s + dy * c) / g.k;
}

// lat/lon → canonical chart pixel of a page rasterised at pageWpx × pageHpx.
// Returns false when the point falls outside the page (caller hides the icon).
inline bool latLonToPagePx(const ChartGeoref& g, double latDeg, double lonDeg,
                           double pageWpx, double pageHpx,
                           double& outX, double& outY)
{
    if (g.k == 0.0 || pageHpx <= 0.0) return false;
    double a, b;
    latLonToPageNorm(g, latDeg, lonDeg, a, b);
    outX =  a * pageHpx;
    outY = -b * pageHpx;
    return outX >= 0.0 && outX <= pageWpx && outY >= 0.0 && outY <= pageHpx;
}

// Direction of true north on the page at this location, radians clockwise from
// page-up.  Computed numerically from two points a small latitude step apart,
// so it follows whatever rotation the georeference carries.
inline bool northAngleOnPage(const ChartGeoref& g, double latDeg, double lonDeg,
                             double& outRad)
{
    double a0, b0, a1, b1;
    latLonToPageNorm(g, latDeg,        lonDeg, a0, b0);
    latLonToPageNorm(g, latDeg + 1e-4, lonDeg, a1, b1);
    const double dx = a1 - a0, dy = -(b1 - b0);   // page y grows down
    if (dx * dx + dy * dy < 1e-24) return false;
    outRad = std::atan2(dx, -dy);                 // clockwise from page-up
    return true;
}

// ── Two-point calibration ───────────────────────────────────────────────────
// Builds a georeference from two (page pixel, lat/lon) correspondences, in the
// same parameterisation ChartFox uses so everything downstream is identical.
// pageHpx is the page height the pixel coordinates belong to.  Returns false
// when the two points are too close together to define a transform.
inline bool georefFromTwoPoints(double x1, double y1, double lat1, double lon1,
                                double x2, double y2, double lat2, double lon2,
                                double pageHpx, ChartGeoref& out)
{
    if (pageHpx <= 0.0) return false;
    const double a1 = x1 / pageHpx, b1 = -y1 / pageHpx;
    const double a2 = x2 / pageHpx, b2 = -y2 / pageHpx;
    const double da = a2 - a1, db = b2 - b1;
    const double dp = std::sqrt(da * da + db * db);

    double m1x, m1y, m2x, m2y;
    latLonToMercator(lat1, lon1, m1x, m1y);
    latLonToMercator(lat2, lon2, m2x, m2y);
    const double dmx = m2x - m1x, dmy = m2y - m1y;
    const double dm = std::sqrt(dmx * dmx + dmy * dmy);
    if (dp < 1e-9 || dm < 1e-6) return false;

    out.transformAngle = std::atan2(dmy, dmx) - std::atan2(db, da);
    out.k = dm / dp;
    const double c = std::cos(out.transformAngle), s = std::sin(out.transformAngle);
    out.tx = m1x - out.k * (a1 * c - b1 * s);
    out.ty = m1y - out.k * (a1 * s + b1 * c);
    return true;
}

}
}
