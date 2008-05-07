#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <click/config.h>
#include <click/userutils.hh>
#include <click/confparse.hh>
#include <click/bitvector.hh>
#include <clicktool/processingt.hh>
#include <clicktool/elementmap.hh>
#include <list>
#include <math.h>
#include <cairo-ps.h>
#include <cairo-pdf.h>
#include "diagram.hh"
#include "dwidget.hh"
#include "wrouter.hh"
#include "whandler.hh"
#include "dstyle.hh"
extern "C" {
#include "support.h"
}
namespace clicky {

extern "C" {
static void diagram_map(GtkWidget *, gpointer);
static gboolean on_diagram_event(GtkWidget *, GdkEvent *, gpointer);
static gboolean diagram_expose(GtkWidget *, GdkEventExpose *, gpointer);
static void on_diagram_size_allocate(GtkWidget *, GtkAllocation *, gpointer);
}

wdiagram::wdiagram(wmain *rw)
    : _rw(rw), _scale_step(0), _scale(1), _origin_x(0), _origin_y(0), _relt(0),
      _layout(false), _drag_state(drag_none)
{
    _widget = lookup_widget(_rw->_window, "diagram");
    gtk_widget_realize(_widget);
    gtk_widget_add_events(_widget, GDK_POINTER_MOTION_MASK
			  | GDK_BUTTON_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK
			  | GDK_LEAVE_NOTIFY_MASK);

    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(_widget->parent);
    _horiz_adjust = gtk_scrolled_window_get_hadjustment(sw);
    _vert_adjust = gtk_scrolled_window_get_vadjustment(sw);

    _base_css_set = _css_set = new dcss_set(dcss_set::default_set("screen"));

#if 0
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    PangoFontFamily **fms;
    int nfms;
    pango_font_map_list_families(fm, &fms, &nfms);
    for (int i = 0; i < nfms; i++)
	fprintf(stderr, "  %s\n", pango_font_family_get_name(fms[i]));
    g_free(fms);
#endif
    _pango_generation = 0;
    _elt_expand = 2;
    
    g_signal_connect(G_OBJECT(_widget), "event",
		     G_CALLBACK(on_diagram_event), this);
    g_signal_connect(G_OBJECT(_widget), "expose-event",
		     G_CALLBACK(diagram_expose), this);
    g_signal_connect(G_OBJECT(_widget), "map",
		     G_CALLBACK(diagram_map), this);
    g_signal_connect(G_OBJECT(_widget), "size-allocate",
		     G_CALLBACK(on_diagram_size_allocate), this);

    for (int i = 0; i < 3; i++)
	_highlight[i].clear();
    static_assert((int) ncursors > (int) deg_corner_lrt
		  && (int) c_element == (int) deg_element);
    for (int i = 0; i < ncursors; i++)
	_cursor[i] = 0;
    _last_cursorno = c_main;
}

wdiagram::~wdiagram()
{
    delete _base_css_set;
    delete _relt;
    _relt = 0;
    for (int i = c_main; i < ncursors; i++)
	if (_cursor[i])
	    gdk_cursor_unref(_cursor[i]);
}

void wdiagram::initialize()
{
    if (!_cursor[c_main]) {
	_cursor[c_main] = _rw->_normal_cursor;
	_cursor[deg_element] = gdk_cursor_new(GDK_HAND1);
	_cursor[deg_corner_ulft] = gdk_cursor_new(GDK_TOP_LEFT_CORNER);
	_cursor[deg_border_top] = gdk_cursor_new(GDK_TOP_SIDE);
	_cursor[deg_corner_urt] = gdk_cursor_new(GDK_TOP_RIGHT_CORNER);
	_cursor[deg_border_rt] = gdk_cursor_new(GDK_RIGHT_SIDE);
	_cursor[deg_corner_lrt] = gdk_cursor_new(GDK_BOTTOM_RIGHT_CORNER);
	_cursor[deg_border_bot] = gdk_cursor_new(GDK_BOTTOM_SIDE);
	_cursor[deg_corner_llft] = gdk_cursor_new(GDK_BOTTOM_LEFT_CORNER);
	_cursor[deg_border_lft] = gdk_cursor_new(GDK_LEFT_SIDE);
	_cursor[c_hand] = gdk_cursor_new(GDK_FLEUR);
	for (int i = c_main; i < ncursors; i++)
	    gdk_cursor_ref(_cursor[i]);
    }
}

String wdiagram::ccss_text() const
{
    return _base_css_set->text();
}

void wdiagram::set_ccss_text(const String &text)
{
    if (_base_css_set->text() && text) {
	delete _base_css_set;
	_base_css_set = _css_set = new dcss_set(dcss_set::default_set("screen"));
	++_pango_generation;
    }
    _base_css_set->parse(text);
}


void wdiagram::display(const String &ename, bool scroll_to)
{
    if (delt *e = _elt_map[ename]) {
	while (!e->root() && e->displayed() <= 0)
	    e = e->parent();
	if (!e->root() && (!e->highlighted(dhlt_click) || scroll_to))
	    highlight(e, dhlt_click, 0, scroll_to);
    }
}

inline void wdiagram::find_rect_elts(const rectangle &r, std::vector<dwidget *> &result) const
{
    _rects.find_all(r, result);
    std::sort(result.begin(), result.end(), dwidget::z_index_less);
    std::vector<dwidget *>::iterator eltsi = std::unique(result.begin(), result.end());
    result.erase(eltsi, result.end());
}

void wdiagram::scroll_recenter(point old_ctr)
{
    if (!_relt)
	return;
    if (old_ctr.x() < -1000000)
	old_ctr = scroll_center();
    
    gtk_layout_set_size(GTK_LAYOUT(_widget),
			MAX((gint) (_relt->_width * _scale + 0.5),
			    _widget->allocation.width),
			MAX((gint) (_relt->_height * _scale + 0.5),
			    _widget->allocation.height));
    
    GtkAdjustment *ha = _horiz_adjust;
    double scaled_width = ha->page_size / _scale;
    if (scaled_width >= _relt->_width) {
	_origin_x = (int) ((_relt->center_x() - scaled_width / 2) * _scale + 0.5);
	gtk_adjustment_set_value(ha, 0);
    } else {
	_origin_x = (int) (_relt->x() * _scale + 0.5);
	if (old_ctr.x() - scaled_width / 2 < _relt->x())
	    gtk_adjustment_set_value(ha, 0);
	else if (old_ctr.x() + scaled_width / 2 > _relt->x2())
	    gtk_adjustment_set_value(ha, _relt->width() * _scale - ha->page_size);
	else
	    gtk_adjustment_set_value(ha, (old_ctr.x() - scaled_width / 2) * _scale - _origin_x);
    }
    
    GtkAdjustment *va = _vert_adjust;
    double scaled_height = va->page_size / _scale;
    if (scaled_height >= _relt->_height) {
	_origin_y = (int) ((_relt->center_y() - scaled_height / 2) * _scale + 0.5);
	gtk_adjustment_set_value(va, 0);
    } else {
	_origin_y = (int) (_relt->y() * _scale + 0.5);
	if (old_ctr.y() - scaled_height / 2 < _relt->y())
	    gtk_adjustment_set_value(va, 0);
	else if (old_ctr.y() + scaled_height / 2 > _relt->y2())
	    gtk_adjustment_set_value(va, _relt->height() * _scale - va->page_size);
	else
	    gtk_adjustment_set_value(va, (old_ctr.y() - scaled_height / 2) * _scale - _origin_y);
    }

    redraw();
}

void wdiagram::zoom(bool incremental, int amount)
{
    if (!incremental && amount <= -10000 && _relt) { // best fit
	// window_width / 3**(scale_step/2) <= contents_width
	// && window_height / 3**(scale_step/2) <= contents_height
	double ssw = 3 * log2(_horiz_adjust->page_size / _relt->width());
	double ssh = 3 * log2(_vert_adjust->page_size / _relt->height());
	_scale_step = (int) floor(std::min(ssw, ssh));
    } else
	_scale_step = (incremental ? _scale_step + amount : amount);
    double new_scale = pow(2.0, _scale_step / 3.0);

    if (_layout) {
	point old_ctr = scroll_center();
	_scale = new_scale;
	_elt_expand = 2;
	scroll_recenter(old_ctr);
    } else
	_scale = new_scale;
}

void wdiagram::router_create(bool incremental, bool always)
{
    if (!incremental) {
	for (int i = 0; i < 3; ++i)
	    unhighlight(i, 0);
	delete _relt;
	_relt = 0;
	_rects.clear();
	_layout = false;
	_elt_map.clear();
    }
    // don't bother creating if widget not mapped
    if (!always && !GTK_WIDGET_VISIBLE(_widget))
	return;
    if (!_relt) {
	_relt = new delt;
	if (_rw->_r) {
	    Vector<ElementT *> path;
	    int z_index = 0;
	    _relt->create_elements(this, _rw->_r, _rw->_processing, _elt_map,
				   path, z_index);
	}
    }
    if (!_cursor[0])
	initialize();
    if (!_layout && _rw->_r) {
	dcontext dcx(this, gtk_widget_create_pango_layout(_widget, NULL), 0);
	dcx.generation = _pango_generation;
	ElementMap::push_default(_rw->element_map());
	_elt_expand = 1;
	_relt->layout_main(dcx);
	g_object_unref(G_OBJECT(dcx.pl));
	scroll_recenter(point(0, 0));
	ElementMap::pop_default();
	_layout = true;
    }
    if (!incremental)
	redraw();
}


/*****
 *
 * Layout
 *
 */

void wdiagram::on_expose(const GdkRectangle *area)
{
    if (!_layout)
	router_create(true, true);

    cairo_t *cr = gdk_cairo_create(GTK_LAYOUT(_widget)->bin_window);
    cairo_rectangle(cr, area->x, area->y, area->width, area->height);
    cairo_clip(cr);

    // background
    cairo_rectangle(cr, area->x, area->y, area->width, area->height);
    //const GdkColor &bgcolor = _widget->style->bg[GTK_STATE_NORMAL];
    //cairo_set_source_rgb(cr, bgcolor.red / 65535., bgcolor.green / 65535., bgcolor.blue / 65535.);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);

    // highlight rectangle
    if (_drag_state == drag_rect_dragging) {
	cairo_set_line_width(cr, 2);
	const GdkColor &bgcolor = _widget->style->bg[GTK_STATE_ACTIVE];
	rectangle r = canvas_to_window(_dragr).normalize();
	if (r.width() > 4 && r.height() > 4) {
	    cairo_set_source_rgb(cr, bgcolor.red / 65535., bgcolor.green / 65535., bgcolor.blue / 65535.);
	    cairo_rectangle(cr, r.x() + 2, r.y() + 2, r.width() - 4, r.height() - 4);
	    cairo_fill(cr);
	}
	cairo_set_source_rgb(cr, bgcolor.red / 80000., bgcolor.green / 80000., bgcolor.blue / 80000.);
	if (r.width() <= 4 || r.height() <= 4) {
	    cairo_rectangle(cr, r.x(), r.y(), r.width(), r.height());
	    cairo_fill(cr);
	} else {
	    cairo_rectangle(cr, r.x() + 1, r.y() + 1, r.width() - 2, r.height() - 2);
	    cairo_stroke(cr);
	}
    }
    
    cairo_translate(cr, -_origin_x, -_origin_y);
    cairo_scale(cr, _scale, _scale);

    dcontext dcx(this, gtk_widget_create_pango_layout(_widget, NULL), cr);
    dcx.generation = _pango_generation;
    dcx.scale_step = _scale_step;

    rectangle r(area->x + _origin_x, area->y + _origin_y,
		area->width, area->height);
    r.scale(1 / _scale);
    r.expand(_elt_expand);
    std::vector<dwidget *> elts;
    find_rect_elts(r, elts);
    for (std::vector<dwidget *>::iterator eltsi = elts.begin();
	 eltsi != elts.end(); ++eltsi)
	(*eltsi)->draw(dcx);
    
    g_object_unref(G_OBJECT(dcx.pl));
    cairo_destroy(cr);
}

extern "C" {
static gboolean diagram_expose(GtkWidget *, GdkEventExpose *e, gpointer user_data)
{
    wdiagram *cd = reinterpret_cast<wdiagram *>(user_data);
    cd->on_expose(&e->area);
    return FALSE;
}

static void diagram_map(GtkWidget *, gpointer user_data)
{
    reinterpret_cast<wdiagram *>(user_data)->router_create(true, true);
}

static void on_diagram_size_allocate(GtkWidget *, GtkAllocation *, gpointer user_data)
{
    wdiagram *cd = reinterpret_cast<wdiagram *>(user_data);
    cd->scroll_recenter(point(-1000001, -1000001));
}
}


/*****
 *
 * export
 *
 */

void wdiagram::export_diagram(const char *filename, bool eps)
{
    if (!_layout)
	router_create(true, true);
    
    cairo_surface_t *crs;
    if (eps) {
	crs = cairo_ps_surface_create(filename, _relt->_width, _relt->_height);
#if CAIRO_VERSION_MINOR >= 6 || (CAIRO_VERSION_MINOR == 5 && CAIRO_VERSION_MICRO >= 2)
	cairo_ps_surface_set_eps(crs, TRUE);
#endif
    } else
	crs = cairo_pdf_surface_create(filename, _relt->_width, _relt->_height);

    cairo_t *cr = cairo_create(crs);
    cairo_translate(cr, -_relt->_x, -_relt->_y);
    dcontext dcx(this, pango_cairo_create_layout(cr), cr);
    dcx.generation = ++_pango_generation;
    dcx.scale_step = 1;		// position precisely
    _css_set = _base_css_set->remedia("print");

    rectangle r(_relt->_x, _relt->_y, _relt->_width, _relt->_height);
    std::vector<dwidget *> elts;
    find_rect_elts(r, elts);
    for (std::vector<dwidget *>::iterator eltsi = elts.begin();
	 eltsi != elts.end(); ++eltsi)
	(*eltsi)->draw(dcx);

    _css_set = _base_css_set;
    g_object_unref(G_OBJECT(dcx.pl));
    cairo_destroy(dcx.cr);
    cairo_surface_destroy(crs);
    ++_pango_generation;
}


/*****
 *
 * handlers
 *
 */

void wdiagram::notify_read(handler_value *hv)
{
    if (delt *e = _elt_map[hv->element_name()])
	e->notify_read(this, hv);
}


/*****
 *
 * reachability
 *
 */

static const char *parse_port(const char *s, const char *end, int &port)
{
    if (s != end && *s == '[') {
	s = cp_integer(cp_skip_space(s + 1, end), end, 10, &port);
	s = cp_skip_space(s, end);
	if (s != end && *s == ']')
	    s = cp_skip_space(s + 1, end);
    }
    return s;
}

static void parse_port(const String &str, String &name, int &port)
{
    const char *s = str.begin(), *end = str.end();
    
    port = -1;
    s = parse_port(s, end, port);
    if (s != end && end[-1] == ']') {
	const char *t = end - 1;
	while (t != s && (isdigit((unsigned char) t[-1])
			  || isspace((unsigned char) t[-1])))
	    --t;
	if (t != s && t[-1] == '[') {
	    parse_port(t - 1, end, port);
	    for (end = t - 1; end != s && isspace((unsigned char) end[-1]); --end)
		/* nada */;
	}
    }
    
    name = str.substring(s, end);
}


wdiagram::reachable_match_t::reachable_match_t(const String &name, int port,
					       bool forward, RouterT *router,
					       ProcessingT *processing)
    : _name(name), _port(port), _forward(forward),
      _router(router), _router_name(), _processing(processing)
{
}

wdiagram::reachable_match_t::reachable_match_t(const reachable_match_t &m,
					       ElementT *e)
    : _name(m._name), _port(m._port), _forward(m._forward),
      _router_name(m._router_name),
      _processing(new ProcessingT(*m._processing, e))
{
    _router = _processing->router();
    if (_router_name)
	_router_name += '/';
    _router_name += e->name();
}

wdiagram::reachable_match_t::~reachable_match_t()
{
    if (_router_name)
	delete _processing;
}

bool
wdiagram::reachable_match_t::get_seed(int eindex, int port) const
{
    if (!_seed.size())
	return false;
    else
	return _seed[_processing->pidx(eindex, port, !_forward)];
}

void
wdiagram::reachable_match_t::set_seed(const ConnectionT &conn)
{
    if (!_seed.size())
	_seed.assign(_processing->npidx(!_forward), false);
    _seed[_processing->pidx(conn, !_forward)] = true;
}

void
wdiagram::reachable_match_t::set_seed_connections(ElementT *e, int port)
{
    assert(port >= -1 && port < e->nports(_forward));
    for (RouterT::conn_iterator cit = _router->begin_connections_touching(PortT(e, port), _forward);
	 cit != _router->end_connections(); ++cit)
	set_seed(*cit);
}

bool
wdiagram::reachable_match_t::add_matches(reachable_t &reach)
{
    for (RouterT::iterator it = _router->begin_elements();
	 it != _router->end_elements(); ++it)
	if (glob_match(it->name(), _name)
	    || glob_match(it->type_name(), _name)
	    || (_name && _name[0] == '#' && glob_match(it->name(), _name.substring(1))))
	    set_seed_connections(it, _port);
	else if (it->resolved_router(_processing->scope())) {
	    reachable_match_t sub_match(*this, it);
	    RouterT *sub_router = sub_match._router;
	    if (sub_match.add_matches(reach)) {
		assert(!reach.compound.get_pointer(sub_match._router_name));
		sub_match.export_matches(reach);
		assert(sub_router->element(0)->name() == "input"
		       && sub_router->element(1)->name() == "output");
		for (int p = 0; p < sub_router->element(_forward)->nports(!_forward); ++p)
		    if (sub_match.get_seed(_forward, p))
			set_seed_connections(it, p);
	    }
	}
    if (_seed.size()) {
	_processing->follow_reachable(_seed, !_forward, _forward);
	return true;
    } else
	return false;
}

void
wdiagram::reachable_match_t::export_matches(reachable_t &reach)
{
    Bitvector &x = (_router_name ? reach.compound[_router_name] : reach.main);
    if (!x.size())
	x.assign(_router->nelements(), false);
    assert(x.size() == _router->nelements());
    if (!_seed.size())
	return;

    for (RouterT::iterator it = _router->begin_elements();
	 it != _router->end_elements(); ++it) {
	if (it->tunnel())
	    continue;
	int pidx = _processing->pidx(it->eindex(), 0, !_forward);
	bool any = false;
	for (int p = 0; p < it->nports(!_forward); ++p)
	    if (_seed[pidx + p]) {
		x[it->eindex()] = any = true;
		break;
	    }
	if (any && it->resolved_router(_processing->scope())) {
	    // spread matches from inputs through the compound
	    reachable_match_t sub_match(*this, it);
	    RouterT *sub_router = sub_match._router;
	    for (int p = 0; p < it->nports(!_forward); ++p)
		if (_seed[pidx + p])
		    sub_match.set_seed_connections(sub_router->element(!_forward), p);
	    if (sub_match._seed.size()) {
		sub_match._processing->follow_reachable(sub_match._seed, !_forward, _forward);
		sub_match.export_matches(reach);
	    }
	}
    }
}

void wdiagram::calculate_reachable(const String &str, bool forward, reachable_t &reach)
{
    String ename;
    int port;
    parse_port(str, ename, port);

    reachable_match_t match(ename, port, forward, _rw->_r, _rw->_processing);
    match.add_matches(reach);
    match.export_matches(reach);
}

/*****
 *
 * motion exposure
 *
 */

void wdiagram::unhighlight(uint8_t htype, rectangle *expose_rect)
{
    assert(htype <= dhlt_pressed);
    while (!_highlight[htype].empty()) {
	delt *e = _highlight[htype].front();
	_highlight[htype].pop_front();
	e->unhighlight(htype);
	e->expose(this, expose_rect);
    }
}

void wdiagram::highlight(delt *e, uint8_t htype, rectangle *expose_rect, bool scroll_to)
{
    if (!e) {
	unhighlight(htype, expose_rect);
	return;
    }

    std::list<delt *>::iterator iter = _highlight[htype].begin();
    if (!e->highlighted(htype) || (++iter, iter != _highlight[htype].end())) {
	unhighlight(htype, expose_rect);
	_highlight[htype].push_front(e);
	e->highlight(htype);
	e->expose(this, expose_rect);
    }

    if (scroll_to && _layout) {
	GtkAdjustment *ha = _horiz_adjust, *va = _vert_adjust;
	rectangle ex = *e;
	ex.expand(e->shadow(this, 0), e->shadow(this, 1),
		  e->shadow(this, 2), e->shadow(this, 3));
	ex = canvas_to_window(ex);
	for (delt *o = e->visible_split(); o && o != e; o = o->split()) {
	    rectangle ox = *o;
	    ox.expand(o->shadow(this, 0), o->shadow(this, 1),
		      o->shadow(this, 2), o->shadow(this, 3));
	    ox = canvas_to_window(ox);
	    rectangle windowrect(ha->value, va->value, ha->page_size, va->page_size);
	    if (ox.intersect(windowrect).area() > ox.intersect(windowrect).area())
		ex = ox;
	}
	
	if (ex.x2() >= ha->value + ha->page_size
	    && ex.x() >= floor(ex.x2() - ha->page_size))
	    gtk_adjustment_set_value(ha, floor(ex.x2() - ha->page_size));
	else if (ex.x2() >= ha->value + ha->page_size
		 || ex.x() < ha->value)
	    gtk_adjustment_set_value(ha, floor(ex.x() - 4));
	
	if (ex.y2() >= va->value + va->page_size
	    && ex.y() >= floor(ex.y2() - va->page_size))
	    gtk_adjustment_set_value(va, floor(ex.y2() - va->page_size));
	else if (ex.y2() >= va->value + va->page_size
		 || ex.y() < va->value)
	    gtk_adjustment_set_value(va, floor(ex.y() - 4));
    }
}

delt *wdiagram::point_elt(const point &p) const
{
    std::vector<dwidget *> elts;
    _rects.find_all(p.x(), p.y(), elts);
    std::sort(elts.begin(), elts.end(), dwidget::z_index_greater);
    std::vector<dwidget *>::iterator eltsi = std::unique(elts.begin(), elts.end());
    elts.erase(eltsi, elts.end());
    for (eltsi = elts.begin(); eltsi != elts.end(); ++eltsi)
	if ((*eltsi)->contains(p))
	    if (delt *e = (*eltsi)->cast_elt())
		if (e->displayed() > 0)
		    return e;
    return 0;
}

void wdiagram::on_drag_motion(const point &p)
{
    point delta = p - _dragr.origin();

    if (_drag_state == drag_start
	&& (fabs(delta.x()) * _scale >= 3
	    || fabs(delta.y()) * _scale >= 3)) {
	for (std::list<delt *>::iterator iter = _highlight[dhlt_click].begin();
	     iter != _highlight[dhlt_click].end(); ++iter)
	    (*iter)->drag_prepare();
	_drag_state = drag_dragging;
    }
    
    if (_drag_state == drag_dragging && _last_cursorno == deg_element) {
	for (std::list<delt *>::iterator iter = _highlight[dhlt_click].begin();
	     iter != _highlight[dhlt_click].end(); ++iter)
	    (*iter)->drag_shift(this, delta);
    } else if (_drag_state == drag_dragging) {
	delt *e = _highlight[dhlt_hover].front();
	e->drag_size(this, delta, _last_cursorno);
    }
}

void wdiagram::on_drag_rect_motion(const point &p)
{
    if (_drag_state == drag_rect_start
	&& (fabs(p.x() - _dragr.x()) * _scale >= 3
	    || fabs(p.y() - _dragr.y()) * _scale >= 3))
	_drag_state = drag_rect_dragging;

    if (_drag_state == drag_rect_dragging) {
	rectangle to_redraw = _dragr.normalize();

	for (std::list<delt *>::iterator iter = _highlight[dhlt_click].begin();
	     iter != _highlight[dhlt_click].end(); )
	    if ((*iter)->highlighted(dhlt_rect_click)) {
		(*iter)->unhighlight(dhlt_rect_click);
		(*iter)->unhighlight(dhlt_click);
		to_redraw |= **iter;
		iter = _highlight[dhlt_click].erase(iter);
	    } else
		++iter;
	
	_dragr._width = p.x() - _dragr.x();
	_dragr._height = p.y() - _dragr.y();

	std::vector<dwidget *> elts;
	find_rect_elts(_dragr.normalize(), elts);
	for (std::vector<dwidget *>::iterator iter = elts.begin();
	     iter != elts.end(); ++iter)
	    if (delt *e = (*iter)->cast_elt())
		if (!e->highlighted(dhlt_click)) {
		    e->highlight(dhlt_rect_click);
		    e->highlight(dhlt_click);
		    _highlight[dhlt_click].push_front(e);
		    to_redraw |= *e;
		}

	to_redraw |= _dragr.normalize();
	redraw(to_redraw);
    }
}

void wdiagram::on_drag_complete()
{
    bool changed = false;
    for (std::list<delt *>::iterator iter = _highlight[dhlt_click].begin();
	 iter != _highlight[dhlt_click].end() && !changed; ++iter)
	changed = (*iter)->drag_canvas_changed(*_relt);

    if (changed) {
	point old_ctr = scroll_center();
	_relt->layout_recompute_bounds();
	scroll_recenter(old_ctr);
    }
}

void wdiagram::on_drag_rect_complete()
{
    std::vector<dwidget *> elts;
    find_rect_elts(_dragr, elts);
    for (std::vector<dwidget *>::iterator iter = elts.begin();
	 iter != elts.end(); ++iter)
	if (delt *e = (*iter)->cast_elt())
	    e->unhighlight(dhlt_rect_click);
    redraw(_dragr.normalize());
}

void wdiagram::on_drag_hand_motion(double x_root, double y_root)
{
    if (_drag_state == drag_hand_start
	&& (fabs(x_root - _dragr.x()) >= 3
	    || fabs(y_root - _dragr.y()) >= 3)) {
	_dragr.set_size(gtk_adjustment_get_value(_horiz_adjust),
			gtk_adjustment_get_value(_vert_adjust));
	_drag_state = drag_hand_dragging;
    }

    if (_drag_state == drag_hand_dragging) {
	double dx = _dragr.x() - x_root;
	double dy = _dragr.y() - y_root;
	gtk_adjustment_set_value(_horiz_adjust, std::min(_dragr.width() + dx, _horiz_adjust->upper - _horiz_adjust->page_size));
	gtk_adjustment_set_value(_vert_adjust, std::min(_dragr.height() + dy, _vert_adjust->upper - _vert_adjust->page_size));
    }
}

void wdiagram::set_cursor(delt *h, double x, double y, int state)
{
    int cnum;
    if (h) {
	int gnum = h->find_gadget(this, x, y);
	cnum = (gnum >= deg_element && gnum < ncursors ? gnum : c_main);
    } else if ((state & GDK_SHIFT_MASK)
	       || (_drag_state != drag_hand_start && _drag_state != drag_hand_dragging))
	cnum = c_main;
    else
	cnum = c_hand;
    if (_last_cursorno != cnum) {
	_last_cursorno = cnum;
	gdk_window_set_cursor(_widget->window, _cursor[_last_cursorno]);
    }
}

gboolean wdiagram::on_event(GdkEvent *event)
{
    if (event->type == GDK_MOTION_NOTIFY) {
	point p = window_to_canvas(event->motion.x, event->motion.y);
	if (!(event->motion.state & GDK_BUTTON1_MASK)) {
	    delt *e = point_elt(p);
	    highlight(e, dhlt_hover, 0, false);
	    set_cursor(e, event->motion.x, event->motion.y, event->motion.state);
	} else if (_drag_state == drag_start || _drag_state == drag_dragging)
	    on_drag_motion(p);
	else if (_drag_state == drag_rect_start || _drag_state == drag_rect_dragging)
	    on_drag_rect_motion(p);
	else if (_drag_state == drag_hand_start || _drag_state == drag_hand_dragging)
	    on_drag_hand_motion(event->motion.x_root, event->motion.y_root);

	// Getting pointer position tells GTK to give us more motion events
	GdkModifierType mod;
	gint mx, my;
	(void) gdk_window_get_pointer(_widget->window, &mx, &my, &mod);

    } else if (event->type == GDK_BUTTON_PRESS && event->button.button == 1) {
	point p = window_to_canvas(event->button.x, event->button.y);
	delt *e = point_elt(p);
	if (e) {
	    _drag_state = drag_start;
	    _dragr.set_origin(p);
	} else if (event->button.state & GDK_SHIFT_MASK) {
	    _drag_state = drag_rect_start;
	    _dragr.set_origin(p);
	} else {
	    _drag_state = drag_hand_start;
	    _dragr.set_origin(event->button.x_root, event->button.y_root);
	}

	if (!(event->button.state & GDK_SHIFT_MASK)) {
	    if (e && !e->highlighted(dhlt_click))
		highlight(e, dhlt_click, 0, false);
	    if (e)
		_rw->element_show(e->flat_name(), 0, true);
	} else if (e && e->highlighted(dhlt_click)) {
	    std::list<delt *>::iterator iter = std::find(_highlight[dhlt_click].begin(), _highlight[dhlt_click].end(), e);
	    _highlight[dhlt_click].erase(iter);
	    e->unhighlight(dhlt_click);
	    _drag_state = drag_none;
	} else if (e) {
	    e->highlight(dhlt_click);
	    _highlight[dhlt_click].push_front(e);
	}

	highlight(e, dhlt_pressed, 0, false);
	set_cursor(e, event->button.x, event->button.y, event->button.state);
	
    } else if ((event->type == GDK_BUTTON_RELEASE && event->button.button == 1)
	       || (event->type == GDK_FOCUS_CHANGE && !event->focus_change.in)) {
	if (_drag_state == drag_dragging)
	    on_drag_complete();
	else if (_drag_state == drag_rect_dragging)
	    on_drag_rect_complete();
	_drag_state = drag_none;
	unhighlight(dhlt_pressed, 0);
	
    } else if (event->type == GDK_2BUTTON_PRESS && event->button.button == 1) {
	delt *e = point_elt(window_to_canvas(event->button.x, event->button.y));
	highlight(e, dhlt_click, 0, true);
	if (e) {
	    _rw->element_show(e->flat_name(), 1, true);
	    _drag_state = drag_start;
	}
	
    } else if (event->type == GDK_BUTTON_RELEASE && event->button.button == 3) {
	/*delt *e = point_elt(window_to_canvas(event->button.x, event->button.y));
	if (e) {
	    rectangle bounds = *e;
	    e->remove(_rects, bounds);
	    e->set_orientation(3 - e->orientation());
	    e->insert(_rects, this, bounds);
	    redraw(bounds);
	    }*/
    }
    
    return FALSE;
}

extern "C" {
static gboolean on_diagram_event(GtkWidget *, GdkEvent *event, gpointer user_data)
{
    wdiagram *cd = reinterpret_cast<wdiagram *>(user_data);
    return cd->on_event(event);
}
}

}