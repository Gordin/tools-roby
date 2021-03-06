#include "../value_set/value_set.hh"
#include "graph.hh"
#include <boost/graph/depth_first_search.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/iterator/filter_iterator.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/bind.hpp>
#include <boost/graph/reverse_graph.hpp>
#include "undirected_graph.hh"
#include "undirected_dfs.hh"
#include <queue>
#include <functional>

#include "reverse_graph_bug_boost148_workaround.inl"

typedef RubyGraph::vertex_iterator	vertex_iterator;
typedef RubyGraph::vertex_descriptor	vertex_descriptor;
typedef RubyGraph::edge_iterator	edge_iterator;
typedef RubyGraph::edge_descriptor	edge_descriptor;

static VALUE graph_view_of(VALUE self)
{ return rb_iv_get(self, "@__bgl_real_graph__"); }

using namespace boost;
using namespace std;

static ID id_new;
static VALUE utilrbValueSet;

template<typename T>
struct Queue : std::queue<T>
{
    T& top() { return this->front(); }
    T const& top() const { return this->front(); }
};

namespace details {
    // Reverse graphs do not have an adjacency_iterator
    template<typename Graph>
    struct vertex_range< boost::reverse_graph<Graph, Graph&>, false>
    {
	typedef typename Graph::adjacency_iterator iterator;
	typedef std::pair<iterator, iterator> range;

	static range get(RubyGraph::vertex_descriptor v, 
		boost::reverse_graph<Graph, Graph&> const& graph) 
	{ return adjacent_vertices(v, graph.m_g); }
    };

    template<typename Graph>
    struct vertex_range< boost::reverse_graph<Graph, Graph const&>, false>
    {
	typedef typename Graph::adjacency_iterator iterator;
	typedef std::pair<iterator, iterator> range;

	static range get(RubyGraph::vertex_descriptor v, 
		boost::reverse_graph<Graph, Graph const&> const& graph) 
	{ return adjacent_vertices(v, graph.m_g); }
    };
}

/* If +key+ is found in +assoc+, returns its value. Otherwise, initializes 
 * +key+ to +default_value+ in +assoc+ and returns it
 */
template<typename Key, typename Value>
Value& get(map<Key, Value>& assoc, Key const& key, Value const& default_value)
{
    typename map<Key, Value>::iterator it = assoc.find(key);
    if (it != assoc.end())
	return it->second;

    tie(it, tuples::ignore) = assoc.insert( make_pair(key, default_value) );
    return it->second;
}

/* If +key+ is found in +assoc+, returns its value. Otherwise, returns +default_value+
 */
template<typename Key, typename Value>
Value const& get(map<Key, Value> const& assoc, Key const& key, Value const& default_value)
{
    typename map<Key, Value>::const_iterator it = assoc.find(key);
    if (it != assoc.end())
	return it->second;

    return default_value;
}

/* ColorMap is a map with default value */
class ColorMap : private map<vertex_descriptor, default_color_type>
{
    template<typename Key, typename Value>
    friend Value& get(map<Key, Value>&, Key const&, Value const&);

    default_color_type const default_value;

    typedef map<vertex_descriptor, default_color_type> Super;

public:

    typedef Super::key_type	key_type;
    typedef Super::value_type	value_type;

    using Super::clear;

    ColorMap()
	: default_value(color_traits<default_color_type>::white()) {}

    default_color_type& operator[](vertex_descriptor key)
    { 
	default_color_type& c = get(*this, key, default_value); 
	return c;
    }

};

typedef list<vertex_descriptor> vertex_list;

struct vertex_recorder : public default_dfs_visitor
{
public:
    ValueSet&  component;
    vertex_recorder( ValueSet& component )
	: component(component) { }

    template<typename G>
    void discover_vertex(vertex_descriptor u, G const& g)
    { component.insert(g[u]); }
};


static ValueSet& rb_to_set(VALUE object)
{
    if (!RTEST(rb_obj_is_kind_of(object, utilrbValueSet)))
	rb_raise(rb_eArgError, "expected a ValueSet");

    ValueSet* result_set;
    Data_Get_Struct(object, ValueSet, result_set);
    return *result_set;
}

/** Converts a std::set<VALUE> into a ValueSet object 
 * After this method, +source+ is empty */
static VALUE set_to_rb(ValueSet& source)
{
    VALUE result = rb_funcall(utilrbValueSet, id_new, 0);
    ValueSet* result_set;
    Data_Get_Struct(result, ValueSet, result_set);

    result_set->swap(source);
    return result;
}

/* Adds in +result+ all components generated by the items in [it, end). We 
 * assume that there is no component which includes more than one item in
 * [it, end) */
template<typename Graph, typename Iterator>
static void graph_components_i(std::list<ValueSet>& result, Graph const& g, Iterator it, Iterator end, bool include_singletons)
{
    ColorMap   colors;

    result.push_front(ValueSet());
    for (; it != end; ++it)
    {
	if (0 == *it) // elements not in +g+  are handled by graph_result_root_descriptor
	    continue;
	if (colors[*it] != color_traits<default_color_type>::white())
	    continue;

	ValueSet& component(*result.begin());
	depth_first_visit(g, *it, vertex_recorder(component), make_assoc_property_map(colors));
	if (component.size() > 1 || include_singletons)
	    result.push_front(ValueSet());
	else
	    component.clear();
    }
    result.pop_front();
}

/** If +v+ is found in +g+, returns the corresponding vertex_descriptor. Otherwise,
 * add a singleton component to +result+ and return NULL.
 */
static vertex_descriptor graph_components_root_descriptor(std::list<ValueSet>& result, VALUE v, VALUE g, bool include_singletons)
{
    vertex_descriptor d;
    bool exists;
    tie(d, exists) = rb_to_vertex(v, g);
    if (! exists)
    {
	if (include_singletons)
	{
	    ValueSet component;
	    component.insert(v);
	    result.push_back(component);
	}
	return NULL;
    }
    return d;
}
template<typename Graph>
static VALUE graph_do_generated_subgraphs(int argc, VALUE* argv, Graph const& g, VALUE self)
{
    VALUE roots = Qnil, include_singletons;
    if (rb_scan_args(argc, argv, "11", &roots, &include_singletons) == 1)
	include_singletons = Qtrue;

    bool with_singletons = RTEST(include_singletons) ? true : false;
    std::list<ValueSet> result;
    if (NIL_P(roots))
    {
	RubyGraph::vertex_iterator it, end;
	tie(it, end) = vertices(g);
	// call graph_components_i with all root vertices
	// in +graph+
	graph_components_i(result, g, 
		make_filter_iterator(
		    bind(
			vertex_has_adjacent_i<Graph, false>, 
			_1, boost::ref(g)
		    ), it, end
		),
		make_filter_iterator(
		    bind(
			vertex_has_adjacent_i<Graph, false>, 
			_1, boost::ref(g)
		    ), end, end
		), with_singletons
	    );
    }
    else
    {
	ValueSet& root_set = rb_to_set(roots);
	ValueSet::const_iterator 
	    begin = root_set.begin(),
	    end   = root_set.end();

	// call graph_components_i with all vertices given in as argument
	graph_components_i(result, g, 
		make_transform_iterator(begin, 
		    bind(graph_components_root_descriptor, boost::ref(result), _1, self, with_singletons)
		),
		make_transform_iterator(end,
		    bind(graph_components_root_descriptor, boost::ref(result), _1, self, with_singletons)
		), with_singletons);
    }

    // Now convert the result into a Ruby array
    VALUE rb_result = rb_ary_new();
    for (std::list<ValueSet>::iterator it = result.begin(); it != result.end(); ++it)
	rb_ary_push(rb_result, set_to_rb(*it));
    return rb_result;
}
/*
 * call-seq:
 *   graph.components(seeds = nil, include_singletons = true)	=> components
 *
 * Returns an array of vertex sets. Each set is a connected component of
 * +graph+. If a list of vertices +seeds+ is provided, returns only the
 * components the vertices are part of. The graph is treated as if it were not
 * directed.
 *
 * If +include_singletons+ is false and +seeds+ is non-nil, then +components+
 * will not include the singleton components { v } where v is in +seeds+
 */
static VALUE graph_components(int argc, VALUE* argv, VALUE self)
{ 
    VALUE seeds, include_singletons;
    rb_scan_args(argc, argv, "02", &seeds, &include_singletons);
    if (argc == 1)
	include_singletons = Qtrue;

    // Compute the connected components
    RubyGraph const& g = graph_wrapped(self);

    typedef std::map<vertex_descriptor, int> ComponentMap;
    ComponentMap component_map;
    ColorMap	 color_map;
    int count = connected_components(utilmm::make_undirected_graph(g),
	    make_assoc_property_map(component_map), 
	    boost::color_map( make_assoc_property_map(color_map) ));

    VALUE ret = rb_ary_new2(count);
    std::vector<bool>  enabled_components;
    std::vector<VALUE> components(count);
    if (0 == argc) 
	enabled_components.resize(count, true);
    else
    {
	enabled_components.resize(count, false);
	ValueSet& seed_set = rb_to_set(seeds);
	for (ValueSet::const_iterator it = seed_set.begin(); it != seed_set.end(); ++it)
	{
	    VALUE rb_vertex = *it;

	    vertex_descriptor v; bool in_graph;
	    tie(v, in_graph) = rb_to_vertex(rb_vertex, self);
	    if (in_graph)
	    {
		int v_c = component_map[v];
		enabled_components[v_c] = true;
	    }
	    else if (RTEST(include_singletons))
		rb_ary_push(ret, rb_ary_new3(1, rb_vertex));
	}
    }

    // Add empty array for all enabled components
    for (int i = 0; i < count; ++i)
    {
	if (! enabled_components[i]) continue;
	VALUE ary = components[i] = rb_ary_new();
	rb_ary_store(ret, i, ary);
    }

    // Add the vertices to their corresponding Ruby component
    for (ComponentMap::const_iterator it = component_map.begin(); it != component_map.end(); ++it)
    {
	int c = it->second;
	if (! enabled_components[c])
	    continue;

	rb_ary_push(components[c], g[it->first]);
    }

    if (argc > 0 && !RTEST(include_singletons))
    {
	// Remove the remaining singletons
	for (int i = 0; i < count; ++i)
	{
	    if (! enabled_components[i])
		continue;
	    if (RARRAY_LEN(components[i]) == 1)
		rb_ary_store(ret, i, Qnil);
	}
    }

    // Remove all unused component slots (disabled components)
    rb_funcall(ret, rb_intern("compact!"), 0);
    return ret;
}

/*
 * call-seq:
 *   undirected_graph.components(seeds = nil, include_singletons = true) => components
 *
 * Returns an array of vertex sets. Each set is a connected component of +graph+. If
 * a list of vertices is provided, returns only the components the vertices are part of.
 * The graph is treated as if it were not directed. It is equivalent to graph.components.
 */
static
VALUE graph_undirected_components(int argc, VALUE* argv, VALUE self)
{ return graph_components(argc, argv, graph_view_of(self)); }

/* call-seq:
 *   graph.generated_subgraph([v1, v2, ...][, include_singletons])		   => components
 *
 * Returns an array of vertex sets. Each set is the component that can be
 * reached from one of the given seed. If no initial vertex is given, the graph
 * roots are taken.
 */
static VALUE graph_generated_subgraphs(int argc, VALUE* argv, VALUE self)
{ return graph_do_generated_subgraphs(argc, argv, graph_wrapped(self), self); }

/* call-seq:
 *   graph.generated_subgraph([v1, v2, ...])		   => components
 *
 * Like Graph#generated_subgraph, but on the reverse graph of +graph+ (where edges has
 * been swapped)
 */
static VALUE graph_reverse_generated_subgraphs(int argc, VALUE* argv, VALUE self)
{ 
    VALUE real_graph = rb_iv_get(self, "@__bgl_real_graph__");
    return graph_do_generated_subgraphs(argc, argv, make_reverse_graph(graph_wrapped(real_graph)), real_graph); 
}

static const int VISIT_TREE_EDGES = 1;
static const int VISIT_BACK_EDGES = 2;
static const int VISIT_FORWARD_OR_CROSS_EDGES = 4;
static const int VISIT_NON_TREE_EDGES = 6;
static const int VISIT_ALL_EDGES = 7;

struct ruby_dfs_visitor : public default_dfs_visitor
{

    int m_mode;
    ruby_dfs_visitor(int mode)
	: m_mode(mode) { } 

    template<typename E, typename G>
    void tree_edge(E e, G const& graph)
    { yield_edge(e, graph, VISIT_TREE_EDGES); }
    template<typename E, typename G>
    void back_edge(E e, G const& graph)
    { yield_edge(e, graph, VISIT_BACK_EDGES); }
    template<typename E, typename G>
    void forward_or_cross_edge(E e, G const& graph)
    { yield_edge(e, graph, VISIT_FORWARD_OR_CROSS_EDGES); }

    template<typename E, typename G>
    void yield_edge(E e, G const& graph, int what)
    {
	if (!(what & m_mode))
	    return;

	VALUE rb_source = graph[source(e, graph)];
	VALUE rb_target = graph[target(e, graph)];
	VALUE info = graph[reverse_graph_bug_boost148_workaround::edge_access<E>::get(e)].info;
	rb_yield_values(4, rb_source, rb_target, info, INT2FIX(what));
    }
};

template<typename G>
static bool search_terminator(vertex_descriptor u, G const& g)
{ 
    VALUE thread = rb_thread_current();
    bool result = RTEST(rb_thread_local_aref(thread, rb_intern("@prune")));
    if (result)
	rb_thread_local_aset(thread, rb_intern("@prune"), Qfalse);
    return result;
}

/* @overload reset_prune_flag
 *   Resets the prune flag. This can be used in algorithms that want to allow
 *   the blocks given to them to use {#prune} as a way to stop iteration without
 *   actually passing it to the underlying iteration algorithm
 */
static VALUE graph_reset_prune_flag(VALUE graph)
{
    rb_thread_local_aset(rb_thread_current(), rb_intern("@prune"), Qfalse);
    return Qnil;
}

/* @overload pruned?
 *   Test if Graph.prune got called in the current graph iteration
 */
static VALUE graph_pruned_p(VALUE graph)
{
    VALUE thread = rb_thread_current();
    return rb_thread_local_aref(thread, rb_intern("@prune"));
}

/* call-seq:
 *  graph.prune
 *
 * In #each_dfs, call this method to stop developing the current branch
 */
static VALUE graph_prune(VALUE self)
{
    VALUE thread = rb_thread_current();
    rb_thread_local_aset(thread, rb_intern("@prune"), Qtrue);
    return Qtrue;
}

template<typename Graph>
static VALUE graph_each_dfs(VALUE self, Graph const& graph, VALUE root, VALUE mode)
{
    rb_thread_local_aset(rb_thread_current(), rb_intern("@prune"), Qfalse);

    vertex_descriptor v; bool exists;
    tie(v, exists) = rb_to_vertex(root, self);
    if (! exists)
	return self;

    map<vertex_descriptor, default_color_type> colors;
    depth_first_visit(graph, v, ruby_dfs_visitor(FIX2INT(mode)), 
	    make_assoc_property_map(colors), &search_terminator<Graph>);
    return self;
}

/* call-seq:
 *  graph.each_dfs(root, mode) { |source, dest, info, kind| ... }
 *
 * Enumerates edges of the graph following a depth-first search order.
 * +mode+ is a filter on the kind of edge which shall be enumerated
 * (BGL::Graph::TREE, BGL::Graph::FORWARD_OR_CROSS, BGL::Graph::BACK and
 * BGL::Graph::ALL) and +root+ is the source of the search
 */
static VALUE graph_direct_each_dfs(VALUE self, VALUE root, VALUE mode)
{
    RubyGraph& graph = graph_wrapped(self);
    return graph_each_dfs(self, graph, root, mode);
}

/* call-seq:
 *  graph.each_dfs(root, mode) { |source, dest, info, kind| ... }
 *
 * Enumerates edges of the graph following a depth-first search order.
 * +mode+ is a filter on the kind of edge which shall be enumerated (TREE,
 * NON_TREE and ALL) and +root+ is the source of the search
 */
static VALUE graph_reverse_each_dfs(VALUE self, VALUE root, VALUE mode)
{
    VALUE real_graph = graph_view_of(self);
    RubyGraph& graph = graph_wrapped(real_graph);
    return graph_each_dfs(real_graph, make_reverse_graph(graph), root, mode);
}

/* call-seq:
 *  graph.each_dfs(root, mode) { |source, dest, info, kind| ... }
 *
 * Enumerates edges of the graph following a depth-first search order.
 * +mode+ is a filter on the kind of edge which shall be enumerated (TREE,
 * FORWARD_OR_CROSS, BACK and ALL) and +root+ is the source of the search
 */
static VALUE graph_undirected_each_dfs(VALUE self, VALUE root, VALUE mode)
{
    VALUE real_graph = graph_view_of(self);
    RubyGraph& graph = graph_wrapped(real_graph);
    typedef utilmm::undirected_graph<RubyGraph> Undirected;
    Undirected undirected(graph);

    vertex_descriptor v; bool exists;
    tie(v, exists) = rb_to_vertex(root, real_graph);
    if (! exists)
	return self;

    ColorMap colors;
    edge_iterator ei, ei_end;
    for(tie(ei, ei_end) = edges(graph); ei != ei_end; ++ei)
	graph[*ei].color = boost::white_color;

    rb_thread_local_aset(rb_thread_current(), rb_intern("@prune"), Qfalse);
    utilmm::undirected_depth_first_visit(undirected, v, ruby_dfs_visitor(FIX2INT(mode)),
	    make_assoc_property_map(colors), 
	    utilmm::make_undirected_edge_map(get(&EdgeProperty::color, graph)), 
	    &search_terminator<Undirected>);
    return self;
}

struct ruby_reachable_visitor : default_dfs_visitor
{
    bool& m_found;
    vertex_descriptor m_target;

    ruby_reachable_visitor(bool& found, vertex_descriptor target)
	: m_found(found), m_target(target) { m_found = false; }

    template<typename E, typename G>
    void tree_edge(E e, G const& graph)
    { 
	if (m_target == target(e, graph))
	    m_found = true;
    }
};

struct ruby_reachable_terminator
{ 
    bool const& found;
    ruby_reachable_terminator(bool const& found)
	: found(found) { }

    template<typename G>
    bool operator()(vertex_descriptor u, G const& g) const { return found; }
};

/* call-seq:
 *  graph.reachable?(v1, v2)
 *
 * Returns true if v2 can be reached from v1
 */
VALUE graph_reachable_p(VALUE self, VALUE source, VALUE target)
{
    RubyGraph& graph = graph_wrapped(self);
    vertex_descriptor s, t; bool exists;
    tie(s, exists) = rb_to_vertex(source, self);
    if (! exists)
	return Qfalse;
    tie(t, exists) = rb_to_vertex(target, self);
    if (! exists)
	return Qfalse;

    map<vertex_descriptor, default_color_type> colors;
    bool found;
    depth_first_visit(graph, s, ruby_reachable_visitor(found, t), 
	    make_assoc_property_map(colors), ruby_reachable_terminator(found));

    return found;
}


struct ruby_bfs_visitor : public default_bfs_visitor
{
    int m_mode;
    ruby_bfs_visitor(int mode)
	: m_mode(mode) { } 

    template<typename E, typename G>
    void tree_edge(E e, G const& graph)
    { yield_edge(e, graph, VISIT_TREE_EDGES); }
    template<typename E, typename G>
    void non_tree_edge(E e, G const& graph)
    { yield_edge(e, graph, VISIT_NON_TREE_EDGES); }
    template<typename E, typename G>
    void yield_edge(E e, G const& graph, int what)
    {
	if (!(what & m_mode))
	    return;

	VALUE source_vertex = graph[source(e, graph)];
	VALUE target_vertex = graph[target(e, graph)];
	VALUE info = graph[reverse_graph_bug_boost148_workaround::edge_access<E>::get(e)].info;
	rb_yield_values(4, source_vertex, target_vertex, info, INT2FIX(what));
    }
};

template<typename Graph>
static VALUE graph_each_bfs(VALUE self, Graph const& graph, VALUE root, VALUE mode)
{
    int intmode = FIX2INT(mode);
    if ((intmode & VISIT_NON_TREE_EDGES) && ((intmode & VISIT_NON_TREE_EDGES) != VISIT_NON_TREE_EDGES))
	rb_raise(rb_eArgError, "cannot use FORWARD_OR_CROSS and BACK");

    vertex_descriptor v; bool exists;
    tie(v, exists) = rb_to_vertex(root, self);
    if (! exists)
	return self;

    rb_thread_local_aset(rb_thread_current(), rb_intern("@prune"), Qfalse);
    map<vertex_descriptor, default_color_type> colors;
    Queue<vertex_descriptor> queue;
    breadth_first_search(graph, v, queue, ruby_bfs_visitor(intmode), 
	    make_assoc_property_map(colors));
    return self;
}

/* call-seq:
 *  graph.each_bfs(root, mode) { |source, dest, info, kind| ... }
 *
 * Enumerates edges of the graph following a breadth-first search order.
 * +mode+ is a filter on the kind of edge which shall be enumerated
 * (BGL::Graph::TREE, BGL::Graph::NON_TREE and BGL::Graph::ALL) and
 * +root+ is the source of the search
 */
static VALUE graph_direct_each_bfs(VALUE self, VALUE root, VALUE mode)
{
    RubyGraph& graph = graph_wrapped(self);
    return graph_each_bfs(self, graph, root, mode);
}

/* call-seq:
 *  graph.each_bfs(root, mode) { |source, dest, info, kind| ... }
 *
 * Enumerates edges of the graph following a breadth-first search order.
 * +mode+ is a filter on the kind of edge which shall be enumerated (TREE,
 * NON_TREE and ALL) and +root+ is the source of the search
 */
static VALUE graph_reverse_each_bfs(VALUE self, VALUE root, VALUE mode)
{
    VALUE real_graph = graph_view_of(self);
    RubyGraph& graph = graph_wrapped(real_graph);
    return graph_each_bfs(real_graph, make_reverse_graph(graph), root, mode);
}

/* call-seq:
 *  graph.each_bfs(root, mode) { |source, dest, info, kind| ... }
 *
 * Enumerates edges of the graph following a breadth-first search order.
 * +mode+ is a filter on the kind of edge which shall be enumerated (TREE,
 * NON_TREE and ALL) and +root+ is the source of the search
 */
static VALUE graph_undirected_each_bfs(VALUE self, VALUE root, VALUE mode)
{
    VALUE real_graph = graph_view_of(self);
    RubyGraph& graph = graph_wrapped(real_graph);
    return graph_each_bfs(real_graph, utilmm::make_undirected_graph(graph), root, mode);
}

/* call-seq:
 *  graph.topological_sort => array
 *
 * Returns a topological sorting of this graph
 */
static VALUE graph_topological_sort(int argc, VALUE* argv, VALUE self)
{
    VALUE rb_result;
    rb_scan_args(argc, argv, "01", &rb_result);
    if (NIL_P(rb_result))
	rb_result = rb_ary_new();
    else
	rb_ary_clear(rb_result);

    RubyGraph& graph = graph_wrapped(self);
    typedef std::vector<RubyGraph::vertex_descriptor> Result;
    Result result;

    map<vertex_descriptor, default_color_type> colors;
    try
    {
	topological_sort(graph, std::back_inserter(result), 
		boost::color_map(make_assoc_property_map(colors)));

	for (int i = result.size() - 1; i >= 0; --i)
	    rb_ary_push(rb_result, graph[result[i]]);
	return rb_result;
    }
    catch(boost::not_a_dag) {}
    rb_raise(rb_eArgError, "the graph is not a DAG");
}

/**********************************************************************
 *  Extension initialization
 */

/*
 * Document-class: BGL
 *
 * The BGL module defines a Graph class and a Vertex module. The Graph class can
 * be used to manipulate graphs where vertices are referenced by a graph descriptor
 * (Graph#add_edge, Graph#add_vertex, ...). However, the preferred way to us BGL is
 * to mix Vertex in the vertex objects and use the associated methods:
 * 
 *   class MyNode
 *     include BGL::Graph
 *   end
 *   graph = Graph.new
 *   v1, v2 = MyNode.new, MyNode.new
 *   graph.link(v1, v2, [])
 *   ...
 *   v1.each_child_object { ... }
 */

/*
 * Document-class: BGL::Graph
 *
 * A directed graph between Ruby objects. See BGL documentation.
 */

/*
 * Document-class: BGL::Vertex
 *
 * A module to be mixed in objects used as vertices in Graph. It
 * allows to use the same object in more than one graph.
 */

void Init_graph_algorithms()
{
    id_new = rb_intern("new");

    bglModule = rb_define_module("BGL");
    bglGraph  = rb_define_class_under(bglModule, "Graph", rb_cObject);
    rb_define_const(bglGraph , "TREE"             , INT2FIX(VISIT_TREE_EDGES));
    rb_define_const(bglGraph , "FORWARD_OR_CROSS" , INT2FIX(VISIT_FORWARD_OR_CROSS_EDGES));
    rb_define_const(bglGraph , "BACK"             , INT2FIX(VISIT_BACK_EDGES));
    rb_define_const(bglGraph , "NON_TREE"         , INT2FIX(VISIT_NON_TREE_EDGES));
    rb_define_const(bglGraph , "ALL"              , INT2FIX(VISIT_ALL_EDGES));

    rb_define_method(bglGraph, "components",   RUBY_METHOD_FUNC(graph_components), -1);
    rb_define_method(bglGraph, "generated_subgraphs",   RUBY_METHOD_FUNC(graph_generated_subgraphs), -1);
    rb_define_method(bglGraph, "each_dfs",	RUBY_METHOD_FUNC(graph_direct_each_dfs), 2);
    rb_define_method(bglGraph, "each_bfs",	RUBY_METHOD_FUNC(graph_direct_each_bfs), 2);
    rb_define_method(bglGraph, "reachable?", RUBY_METHOD_FUNC(graph_reachable_p), 2);
    rb_define_method(bglGraph, "prune",		RUBY_METHOD_FUNC(graph_prune), 0);
    rb_define_method(bglGraph, "pruned?",       RUBY_METHOD_FUNC(graph_pruned_p), 0);
    rb_define_method(bglGraph, "reset_prune",       RUBY_METHOD_FUNC(graph_reset_prune_flag), 0);
    rb_define_method(bglGraph, "topological_sort",		RUBY_METHOD_FUNC(graph_topological_sort), -1);

    bglReverseGraph = rb_define_class_under(bglGraph, "Reverse", rb_cObject);
    rb_define_method(bglReverseGraph, "generated_subgraphs",RUBY_METHOD_FUNC(graph_reverse_generated_subgraphs), -1);
    rb_define_method(bglReverseGraph, "each_dfs",   RUBY_METHOD_FUNC(graph_reverse_each_dfs), 2);
    rb_define_method(bglReverseGraph, "each_bfs",   RUBY_METHOD_FUNC(graph_reverse_each_bfs), 2);
    rb_define_method(bglReverseGraph, "prune",	    RUBY_METHOD_FUNC(graph_prune), 0);

    bglUndirectedGraph = rb_define_class_under(bglGraph, "Undirected", rb_cObject);
    rb_define_method(bglUndirectedGraph, "generated_subgraphs",  RUBY_METHOD_FUNC(graph_undirected_components), -1);
    rb_define_method(bglUndirectedGraph, "each_dfs",	RUBY_METHOD_FUNC(graph_undirected_each_dfs), 2);
    rb_define_method(bglUndirectedGraph, "each_bfs",	RUBY_METHOD_FUNC(graph_undirected_each_bfs), 2);
    rb_define_method(bglUndirectedGraph, "prune",	RUBY_METHOD_FUNC(graph_prune), 0);

    utilrbValueSet = rb_define_class("ValueSet", rb_cObject);
}

