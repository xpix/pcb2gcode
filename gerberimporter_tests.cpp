#define BOOST_TEST_MODULE gerberimporter tests
#include <boost/test/included/unit_test.hpp>

#include "gerberimporter.hpp"
#include <sys/types.h>
#include <dirent.h>
#include <glibmm/init.h>
#include <gdkmm/wrap_init.h>
#include <gdkmm/pixbuf.h>
#include <sstream>
#include <librsvg-2.0/librsvg/rsvg.h>
//#include <ImageMagick-6/Magick++.h>
#include <boost/format.hpp>

struct Fixture {
  Fixture() {
    Glib::init();
    Gdk::wrap_init();
    //MagickCore::InitializeMagick(boost::unit_test::framework::master_test_suite().argv[0]);
  }
  ~Fixture() {}
};

BOOST_FIXTURE_TEST_SUITE(gerberimporter_tests, Fixture);

const unsigned int dpi = 1000;
const unsigned int procmargin = 0;

class Grid {
 public:
  Grid() {}
  Grid(const Cairo::RefPtr<Cairo::ImageSurface> cairo_surface, char background, char foreground) {
    guint8* pixels = cairo_surface->get_data();
    int stride = cairo_surface->get_stride();
    grid.resize(cairo_surface->get_height());
    for (int y = 0; y < cairo_surface->get_height(); y++) {
      grid[y].resize(cairo_surface->get_width());
      for (int x = 0; x < cairo_surface->get_width(); x++) {
        if (*(reinterpret_cast<const uint32_t *>(pixels + x*4 + y*stride)) == 0xFF000000) {
          grid[y][x] = background;
        } else {
          grid[y][x] = foreground;
        }
      }
    }
  }
  const vector<vector<char>> get_grid() const {
    return grid;
  }
  void write_xpm3(std::ostream& os, const vector<string>& colors) const {
    os << "/* XPM */" << std::endl;
    os << "static char * grid_xpm[] = {" << std::endl;
    os << '"' << ((grid.size() > 0) ? grid[0].size()  : 0) << " "; // width
    os << grid.size() << " "; //height
    os << colors.size() << " "; // color count
    os << "1\"," << std::endl; // One character per pixel
    for (const auto& color : colors) {
      os << '"' << color << "\"," << std::endl;
    }
    for (const auto& row : grid) {
      os << '"';
      for (const auto& c : row) {
        os << c;
      }
      os << "\"," << std::endl;
    }
    os << '}' << std::endl;
  }
  Grid& operator|=(const Grid& rhs) {
    for (unsigned int i = 0; i < grid.size() && i < rhs.grid.size(); i++) {
      for (unsigned int j = 0; j < grid[i].size() && j < rhs.grid[i].size(); j++) {
        this->grid[i][j] |= rhs.grid[i][j];
      }
    }
    return *this;
  }
  void remove_row() {
    grid.erase(grid.cbegin());
  }
  map<char, unsigned int> get_counts() const {
    map<char, unsigned int> counts;
    for (unsigned int i = 0; i < grid.size(); i++) {
      for (unsigned int j = 0; j < grid[i].size(); j++) {
        counts[this->grid[i][j]]++;
      }
    }
    return counts;
  }

 private:
  vector<vector<char>> grid;
};

// Make a surface of the right size for the input gerber.
Cairo::RefPtr<Cairo::ImageSurface> create_cairo_surface(const GerberImporter& g) {
  Cairo::RefPtr<Cairo::ImageSurface> cairo_surface =
      Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32,
                                  g.get_width()  * dpi + 1,
                                  g.get_height() * dpi + 1);
  // Set it all black.
  guint8* pixels = cairo_surface->get_data();
  int stride = cairo_surface->get_stride();
  for (int y = 0; y < cairo_surface->get_height(); y++) {
    for (int x = 0; x < cairo_surface->get_width(); x++) {
      *(reinterpret_cast<uint32_t *>(pixels + x*4 + y*stride)) = 0xFF000000; // BLACK
    }
  }
  return cairo_surface;
}

// Given a gerber file, return a pixmap that is a rasterized version of that
// gerber.  Uses gerbv's built-in utils.
Grid bitmap_from_gerber(const GerberImporter& g) {
  Cairo::RefPtr<Cairo::ImageSurface> cairo_surface = create_cairo_surface(g);

  //Render
  g.render(cairo_surface, dpi, g.get_min_x(), g.get_min_y());

  // Make the grid.
  return Grid(cairo_surface, '.', 'O');
}

string render_svg(const GerberImporter& g) {
  multi_polygon_type_fp polys = g.render(false, 30);
  std::stringstream svg_stream;
  {
    box_type_fp svg_bounding_box;
    bg::envelope(polys, svg_bounding_box);
    const double width = (svg_bounding_box.max_corner().x() - svg_bounding_box.min_corner().x()) * dpi / g.vectorial_scale();
    const double height = (svg_bounding_box.max_corner().y() - svg_bounding_box.min_corner().y()) * dpi / g.vectorial_scale();
    const string svg_dimensions =
        str(boost::format("width=\"%1%\" height=\"%2%\" viewBox=\"0 0 %1% %2%\"") % width % height);
    bg::svg_mapper<point_type_fp> svg(svg_stream,
                                      width,
                                      height,
                                      svg_dimensions);
    svg.add(svg_bounding_box); // This is needed for the next line to work, not sure why.
    svg.map(polys, "fill-opacity:1.0;fill:rgb(255,255,255);");
  } // The svg file is complete when it goes out of scope.
  return svg_stream.str();
}

// Convert the gerber to a boost geometry and then conver that to SVG and then rasterize to a bitmap.
Grid boost_bitmap_from_gerber(const GerberImporter& g) {
  string svg_string = render_svg(g);

  //Now we have the svg, let's make a cairo surface like above.
  GError* gerror = nullptr;
  RsvgHandle *rsvg_handle = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(svg_string.c_str()),
                                                      svg_string.size(),
                                                      &gerror);
  Cairo::RefPtr<Cairo::ImageSurface> cairo_surface = create_cairo_surface(g);
  Cairo::RefPtr<Cairo::Context> cr = Cairo::Context::create(cairo_surface);
  if(!rsvg_handle_render_cairo(rsvg_handle, cr->cobj())) {
    printf("to cairo failed\n");
    exit(2);
  }

  return Grid(cairo_surface, '.', 'X');
}

const string gerber_directory = "testing/gerberimporter";

void test_one(const string& gerber_file, unsigned int expected_errors) {
  string gerber_path = gerber_directory;
  gerber_path += "/";
  gerber_path += gerber_file;
  auto g = GerberImporter(gerber_path);
  Grid grid = bitmap_from_gerber(g);
  Grid grid2 = boost_bitmap_from_gerber(g);
  // These seem to help a little with the alignment:
  grid.remove_row();
  grid.remove_row();
  grid |= grid2;
  auto counts = grid.get_counts();
  unsigned int errors = counts['~'] + counts['o'];
  unsigned int total = errors + counts['.'] + counts['_'];
  BOOST_CHECK_EQUAL(errors, expected_errors);
  std::cout << gerber_file << " error rate: " << errors << "/" << total << "(" << double(errors)/total*100 << "%)" << std::endl;
  if (errors != expected_errors) {
    std::ofstream xpm_out;
    xpm_out.open(str(boost::format("%s.xpm") % gerber_file).c_str());
    grid.write_xpm3(xpm_out,
                    { ". c #000000",
                      "~ c #FF0000",
                      "_ c #003300",
                      "o c #0000FF"
                    });
    xpm_out.close();
  }
  if (errors < expected_errors) {
    std::cout << "This test improved, updates the expectation for "
              << gerber_file << " to " << errors << "." << std::endl;
  }
  if (errors > expected_errors) {
    std::cout << "This test got worse.  Check the xpm file and, if it looks okay, then update the expectation for "
              << gerber_file << " to " << errors << "." << std::endl;
  }
}

BOOST_AUTO_TEST_CASE(all_gerbers) {
  test_one("am-test.gbx", 63189);
  test_one("multivibrator-B.Cu.gbr", 18595);
}

BOOST_AUTO_TEST_SUITE_END()
