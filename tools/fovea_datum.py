"""The database's frame (D-037) for the checkers: WGS 84 (G2139) at an
epoch to the sources' NAD83(2011) and back, by PROJ through GDAL, with no
ballpark transformation (which would change nothing). Point by point,
independently of the compiler's interpolated grid (datum.hpp)."""

from osgeo import osr

osr.UseExceptions()

FRAME = "WGS 84 (G2139)"  # as the manifest names it


class Datum:
    def __init__(self, epoch):
        self.epoch = epoch
        wgs, nad = osr.SpatialReference(), osr.SpatialReference()
        wgs.ImportFromEPSG(9754)
        nad.ImportFromEPSG(6319)
        for srs in (wgs, nad):
            srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        options = osr.CoordinateTransformationOptions()
        options.SetBallparkAllowed(False)
        self.to_nad = osr.CoordinateTransformation(wgs, nad, options)
        self.to_wgs = osr.CoordinateTransformation(nad, wgs, options)

    def to_nad83(self, lat, lon):
        """A WGS-84 point's NAD83(2011) latitude and longitude, and what
        to add to a NAD83 ellipsoid height there to make it WGS-84's."""
        lon_n, lat_n, z, _ = self.to_nad.TransformPoint(lon, lat, 0.0, self.epoch)
        return lat_n, lon_n, -z

    def to_nad83_points(self, lon, lat):
        """Many WGS-84 points (longitudes, latitudes) as NAD83(2011)
        (longitude, latitude) pairs."""
        pts = self.to_nad.TransformPoints([(x, y, 0.0, self.epoch) for x, y in zip(lon, lat)])
        return [(p[0], p[1]) for p in pts]

    def to_wgs84(self, lat, lon):
        lon_w, lat_w, _, _ = self.to_wgs.TransformPoint(lon, lat, 0.0, self.epoch)
        return lat_w, lon_w


def check_frame(manifest, datum, where):
    """None if the manifest's frame is the settings', else why not."""
    frame = manifest.get("frame")
    if frame is None or frame.get("name") != FRAME or frame.get("epoch") != datum.epoch:
        return (f"{where}: the manifest's [frame] is {frame}, the settings' {FRAME} at "
                f"{datum.epoch}: recompile it")
    return None
