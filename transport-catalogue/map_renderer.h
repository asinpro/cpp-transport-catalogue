#pragma once
#include "svg.h"
#include "domain.h"
#include "geo.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <ratio>
#include <vector>
#include <numeric>
#include <set>

namespace renderer {

struct RenderSettings {
    double width = 0.0;
    double height = 0.0;

    double padding = 0.0;

    double line_width = 0.0;
    double stop_radius = 0.0;

    int bus_label_font_size = 0;
    svg::Point bus_label_offset;

    int stop_label_font_size = 0;
    svg::Point stop_label_offset;

    svg::Color underlayer_color;
    double underlayer_width = 0.0;

    std::vector<svg::Color> color_palette;
};

inline const double EPSILON = 1e-6;
inline bool IsZero(double value) {
    return std::abs(value) < EPSILON;
}

class SphereProjector {
public:
    // points_begin и points_end задают начало и конец интервала элементов geo::Coordinates
    template <typename PointInputIt>
    SphereProjector(PointInputIt points_begin, PointInputIt points_end,
                    double max_width, double max_height, double padding)
        : padding_(padding) //
    {
        // Если точки поверхности сферы не заданы, вычислять нечего
        if (points_begin == points_end) {
            return;
        }

        // Находим точки с минимальной и максимальной долготой
        const auto [left_it, right_it] = std::minmax_element(
            points_begin, points_end,
            [](auto lhs, auto rhs) { return lhs.lng < rhs.lng; });
        min_lon_ = left_it->lng;
        const double max_lon = right_it->lng;

        // Находим точки с минимальной и максимальной широтой
        const auto [bottom_it, top_it] = std::minmax_element(
            points_begin, points_end,
            [](auto lhs, auto rhs) { return lhs.lat < rhs.lat; });
        const double min_lat = bottom_it->lat;
        max_lat_ = top_it->lat;

        // Вычисляем коэффициент масштабирования вдоль координаты x
        std::optional<double> width_zoom;
        if (!IsZero(max_lon - min_lon_)) {
            width_zoom = (max_width - 2 * padding) / (max_lon - min_lon_);
        }

        // Вычисляем коэффициент масштабирования вдоль координаты y
        std::optional<double> height_zoom;
        if (!IsZero(max_lat_ - min_lat)) {
            height_zoom = (max_height - 2 * padding) / (max_lat_ - min_lat);
        }

        if (width_zoom && height_zoom) {
            // Коэффициенты масштабирования по ширине и высоте ненулевые,
            // берём минимальный из них
            zoom_coeff_ = std::min(*width_zoom, *height_zoom);
        } else if (width_zoom) {
            // Коэффициент масштабирования по ширине ненулевой, используем его
            zoom_coeff_ = *width_zoom;
        } else if (height_zoom) {
            // Коэффициент масштабирования по высоте ненулевой, используем его
            zoom_coeff_ = *height_zoom;
        }
    }

    // Проецирует широту и долготу в координаты внутри SVG-изображения
    svg::Point operator()(geo::Coordinates coords) const {
        return {
            (coords.lng - min_lon_) * zoom_coeff_ + padding_,
            (max_lat_ - coords.lat) * zoom_coeff_ + padding_
        };
    }

private:
    double padding_;
    double min_lon_ = 0;
    double max_lat_ = 0;
    double zoom_coeff_ = 0;
};

class MapRenderer {
public:
    explicit MapRenderer(RenderSettings settings);

    const RenderSettings& GetSetings() const;

    template<typename BusIterator>
    svg::Document RenderMap(BusIterator first, BusIterator last) const;

private:
    template<typename BusIterator>
    void RenderRoutes(BusIterator first, BusIterator last, const SphereProjector& projector, svg::Document& document) const;

    template<typename StopIterator>
    void RenderStops(StopIterator first, StopIterator last, const SphereProjector& projector, svg::Document& document) const;

    svg::Polyline RenderRouteLine(transport_catalogue::BusPtr bus, const svg::Color& color, const SphereProjector& projector) const;

    void RenderRouteName(const svg::Point& position, const svg::Color& color, const std::string& name, std::vector<svg::Text>& out_texts) const;

    void RenderStopName(const svg::Point& position, const std::string& name, svg::Document& document) const;

    const RenderSettings settings_;
};

template<typename BusIterator>
svg::Document MapRenderer::RenderMap(BusIterator first, BusIterator last) const {
    using namespace std;
    using namespace transport_catalogue;

    set<StopPtr, StopComparator> stops;
    for (auto it = first; it != last; ++it) {
        stops.insert((*it)->stops.begin(), (*it)->stops.end());
    }

    vector<geo::Coordinates> points(stops.size());
    transform(
        stops.begin(), stops.end(),
        points.begin(),
        [](const StopPtr stop){
            return stop->coordinates;
        }
    );

    SphereProjector projector(
        points.begin(), points.end(), settings_.width,
        settings_.height, settings_.padding
    );

    svg::Document document;
    RenderRoutes(first, last, projector, document);
    RenderStops(stops.begin(), stops.end(), projector, document);
    return document;
}

template<typename BusIterator>
void MapRenderer::RenderRoutes(BusIterator first, BusIterator last, const SphereProjector& projector, svg::Document& document) const {
    using namespace svg;
    using namespace std;

    size_t index = 0;
    size_t nums = settings_.color_palette.size();
    vector<Text> route_names;

    for (auto it = first; it != last; ++it) {
        const auto& color = settings_.color_palette[index++ % nums];
        transport_catalogue::BusPtr bus = *it;

        document.Add(RenderRouteLine(bus, color, projector));

        RenderRouteName(projector(bus->stops.front()->coordinates), color, bus->name, route_names);
        if (!bus->is_roundtrip && bus->stops.front() != bus->stops.back()) {
            RenderRouteName(projector(bus->stops.back()->coordinates), color, bus->name, route_names);
        }
    }

    for (const auto& name : route_names) {
        document.Add(name);
    }
}

template<typename StopIterator>
void MapRenderer::RenderStops(StopIterator first, StopIterator last, const SphereProjector& projector, svg::Document& document) const {
    using namespace svg;
    using namespace std;

    for (auto it = first; it != last; ++it) {
        transport_catalogue::StopPtr stop = *it;
        document.Add(Circle()
            .SetCenter(projector(stop->coordinates))
            .SetRadius(settings_.stop_radius)
            .SetFillColor("white"s));
    }

    for (auto it = first; it != last; ++it) {
        transport_catalogue::StopPtr stop = *it;
        RenderStopName(projector(stop->coordinates), stop->name, document);
    }
}

} // namespace renderer
