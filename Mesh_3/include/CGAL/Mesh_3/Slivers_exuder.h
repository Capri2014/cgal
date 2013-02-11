// Copyright (c) 2004-2007  INRIA Sophia-Antipolis (France).
// Copyright (c) 2008 GeometryFactory, Sophia Antipolis (France)
// Copyright (c) 2009  INRIA Sophia-Antipolis (France).
// All rights reserved.
//
// This file is part of CGAL (www.cgal.org).
// You can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// Licensees holding a valid commercial license may use this file in
// accordance with the commercial license agreement provided with the software.
//
// This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
// WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
//
// $URL$
// $Id$
//
//
// Author(s)     : Laurent Rineau, Stephane Tayeb

#ifndef CGAL_MESH_3_SLIVERS_EXUDER_H
#define CGAL_MESH_3_SLIVERS_EXUDER_H

#include <CGAL/Mesh_3/config.h>

#include <CGAL/Double_map.h>
#include <CGAL/iterator.h>
#include <CGAL/Mesh_3/C3T3_helpers.h>
#include <CGAL/Mesh_3/Locking_data_structures.h>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include <iomanip> // std::setprecision
#include <iostream> // std::cerr/cout
#include <boost/iterator/transform_iterator.hpp>
#include <boost/bind.hpp>
#include <boost/format.hpp>

#include <CGAL/Timer.h>

#include <CGAL/Mesh_3/sliver_criteria.h>
#include <CGAL/Mesh_optimization_return_code.h>
#include <CGAL/Mesh_3/Null_exuder_visitor.h>

#ifdef CGAL_CONCURRENT_MESH_3_PROFILING
# define CGAL_PROFILE
# include <CGAL/Profile_counter.h>
#endif

#ifdef CGAL_LINKED_WITH_TBB
# include <tbb/task.h>
#endif


#ifdef CGAL_MESH_3_VERBOSE
  #define CGAL_MESH_3_EXUDER_VERBOSE
#endif


namespace CGAL {
  
namespace Mesh_3 {
  
  namespace details { // various function objects
    
    // That functor Second_of takes a pair as input (the value type of a
    // map), and returns the ".second" member of that pair. It is used in
    // Slivers_exuder, to constructor a transform iterator.
    
    // It should be doable using STL bind operators, but i am not sure how
    // to use them. -- Laurent Rineau, 2007/07/27
    template <typename Map>
    struct Second_of :
    public std::unary_function<typename Map::value_type,
    const typename Map::mapped_type&>
    {
      typedef std::unary_function<typename Map::value_type,
        const typename Map::mapped_type&> Base;
      typedef typename Base::result_type result_type;
      typedef typename Base::argument_type argument_type;
      
      const typename Map::mapped_type&
      operator()(const typename Map::value_type& p) const
      {
        return p.second;
      }
    }; // end class Second_of
    
    // That function is constructed with a vertex handle v1.
    // Then, its operator() takes an other vertex handle v2 as input, and
    // returns the distance d(v1, v2).
    // It is used in Slivers_exuder, to constructor a transform iterator.
    template <typename Gt, typename Vertex_handle>
    class Min_distance_from_v :
    public std::unary_function<Vertex_handle, void>
    {
      const Vertex_handle * v;
      Gt gt;
      double & dist;

    public:
      Min_distance_from_v(const Vertex_handle& vh,
                          double& dist,
                          Gt geom_traits = Gt())
        : v(&vh), gt(geom_traits), dist(dist)
      {
      }
      
      void
      operator()(const Vertex_handle& vh) const
      {
        typedef typename Gt::Compute_squared_distance_3
        Compute_squared_distance_3;
        Compute_squared_distance_3 distance =
        gt.compute_squared_distance_3_object();
        
        const double d = CGAL::to_double(distance((*v)->point(), vh->point()));
        if(d < dist){
          dist = d;
        }
      }
    }; // end class Min_distance_from_v
    
  } // end namespace details
  
  

/************************************************
// Class Slivers_exuder_base
// Two versions: sequential / parallel
************************************************/

// Sequential
template <typename Tr, typename Concurrency_tag>
class Slivers_exuder_base
{
protected:
  typedef typename Tr::Vertex_handle                        Vertex_handle;
  typedef typename Tr::Cell_handle                          Cell_handle;
  typedef typename Tr::Geom_traits                          Gt;
  typedef typename Gt::FT                                   FT;
  typedef typename std::vector<Vertex_handle>               Bad_vertices_vector;

  Slivers_exuder_base(const Bbox_3 &, int) {}

  LockDataStructureType *get_lock_data_structure()  const { return 0; }
  void unlock_all_elements()                        const {}
  void create_root_task()                           const {}
  bool flush_work_buffers()                         const { return true; }
  void wait_for_all()                               const {}
  void destroy_root_task()                          const {}
  template <typename Func>
  void enqueue_work(Func, double)                   const {}

protected:
  /**
   * A functor to remove one \c Cell_handle from a priority queue
   */
  template <typename Tet_priority_queue>
  class Erase_from_queue
  {
  public:
    Erase_from_queue(Tet_priority_queue& queue)
    : r_queue_(queue) { }
    
    void operator()(const Cell_handle& cell)
    { r_queue_.erase(cell); }
    
  private:
    Tet_priority_queue& r_queue_;
  };
  
};

#ifdef CGAL_LINKED_WITH_TBB
// Parallel
template <typename Tr>
class Slivers_exuder_base<Tr, Parallel_tag>
{
protected:
  typedef typename Tr::Vertex_handle                        Vertex_handle;
  typedef typename Tr::Cell_handle                          Cell_handle;
  typedef typename Tr::Geom_traits                          Gt;
  typedef typename Gt::FT                                   FT;
  typedef typename tbb::concurrent_vector<Vertex_handle>    Bad_vertices_vector;

  Slivers_exuder_base(const Bbox_3 &bbox, int num_grid_cells_per_axis)
    : m_lock_ds(bbox, num_grid_cells_per_axis)
    , m_worksharing_ds(bbox)
  {
  }

  LockDataStructureType *get_lock_data_structure() const
  {
    return &m_lock_ds;
  }

  void unlock_all_elements() const
  {
    m_lock_ds.unlock_all_tls_locked_cells();
  }

  void create_root_task() const
  {
    m_empty_root_task = new( tbb::task::allocate_root() ) tbb::empty_task;
    m_empty_root_task->set_ref_count(1);
  }

  bool flush_work_buffers() const
  {
    m_empty_root_task->set_ref_count(1);
    bool keep_flushing = m_worksharing_ds.flush_work_buffers(*m_empty_root_task);
    wait_for_all();
    return keep_flushing;
  }

  void wait_for_all() const
  {
    m_empty_root_task->wait_for_all();
  }

  void destroy_root_task() const
  {
    tbb::task::destroy(*m_empty_root_task);
    m_empty_root_task = 0;
  }

  template <typename Func>
  void enqueue_work(Func f, double value) const
  {
    CGAL_assertion(m_empty_root_task != 0);
    m_worksharing_ds.enqueue_work(f, value, *m_empty_root_task);
  }

public:

protected:
  
  /**
   * A functor to remove one \c Cell_handle from a priority queue
   */
  template <typename Tet_priority_queue>
  class Erase_from_queue
  {
  public:
    Erase_from_queue(Tet_priority_queue&) {}
    
    void operator()(const Cell_handle& cell)
    { cell->increment_erase_counter(); }
  };
  
  mutable LockDataStructureType         m_lock_ds;
  mutable Mesh_3::Auto_worksharing_ds   m_worksharing_ds;
  mutable tbb::task                    *m_empty_root_task;
};
#endif // CGAL_LINKED_WITH_TBB


/************************************************
// Class Slivers_exuder
************************************************/
 
template <
  typename C3T3,
  typename MeshDomain,
  typename SliverCriteria,
  typename Visitor_ = Null_exuder_visitor<C3T3>,
  typename FT = typename C3T3::Triangulation::Geom_traits::FT
  >
class Slivers_exuder
: public Slivers_exuder_base<typename C3T3::Triangulation, 
                             typename C3T3::Concurrency_tag>
{
  // Types
  typedef typename C3T3::Concurrency_tag Concurrency_tag;
  
  typedef Slivers_exuder<C3T3, SliverCriteria, Visitor_, FT> Self;
  typedef Slivers_exuder_base<
    typename C3T3::Triangulation, Concurrency_tag>           Base;
  
  typedef typename C3T3::Triangulation Tr;
  typedef typename Tr::Weighted_point Weighted_point;
  typedef typename Tr::Bare_point Bare_point;
  typedef typename Tr::Cell_handle Cell_handle;
  typedef typename Tr::Facet Facet;
  typedef typename Tr::Vertex_handle Vertex_handle;
  typedef typename Weighted_point::Weight Weight;
  
  typedef typename Tr::Geom_traits Geom_traits;
  typedef typename Geom_traits::Tetrahedron_3 Tetrahedron_3;
  
  typedef typename C3T3::Cells_in_complex_iterator Cell_iterator;
  typedef std::vector<Cell_handle> Cell_vector;
  typedef std::vector<Facet> Facet_vector;
  
  typedef typename C3T3::Surface_patch_index Surface_patch_index;
  typedef typename C3T3::Subdomain_index Subdomain_index;
  typedef typename C3T3::Index Index;
  
  // Umbrella will store the surface_index of internal facets of a new
  // weighted point conflict zone. Such facets are represented by their edge
  // which do not contain the pumped vertex
  typedef std::pair<Vertex_handle,Vertex_handle> Ordered_edge;
  typedef std::map<Ordered_edge, Surface_patch_index > Umbrella;
  
  // Boundary_facets_from_outside represents the facet of the conflict zone
  // seen from outside of it. It stores Surface_patch_index of the facet, and
  // Subdomain_index of the cell which is inside the conflict zone. 
  typedef std::map<Facet, std::pair<Surface_patch_index,Subdomain_index> >
    Boundary_facets_from_outside;
  
  /** Pre_star will represent the pre-star of a point. It is a (double)-map
   *  of Facet (viewed from cells inside the star), ordered by the
   *  critial_radius of the point with the cell that lies on the facet, at
   *  the exterior of the pre-star. */
  typedef CGAL::Double_map<Facet, double> Pre_star;
  
  // Stores the value of facet for the sliver criterion functor
  typedef std::map<Facet, double> Sliver_values;
  
  // A priority queue ordered on Tet quality (SliverCriteria)
  typedef CGAL::Double_map<Cell_handle, double> Tet_priority_queue;
  
  // Visitor class
  // Should define
  //  - after_cell_pumped(std::size_t cells_left_number)
  typedef Visitor_ Visitor;
  
  // Helper
  typedef class C3T3_helpers<C3T3,MeshDomain> C3T3_helpers;

  using Base::get_lock_data_structure;
    
public: // methods
  
  /**
   * @brief Constructor
   * @param c3t3 The mesh to exude
   * @param criteria The criterion which will be used to evaluate tet quality
   * @param d defines the maximal weight we will try:
   * max_weight(v) < d*dist(v,nearest_vertice(v))
   */
  Slivers_exuder(C3T3& c3t3,
                 const MeshDomain& domain,
                 const SliverCriteria& criteria = SliverCriteria(),
                 double d = 0.45);
  
  /**
   * @brief pumps vertices
   * @param criterion_value_limit All vertices of tetrahedra that have a
   * quality below this bound will be pumped
   */
  Mesh_optimization_return_code
  operator()(double criterion_value_limit = SliverCriteria::default_value,
             Visitor visitor = Visitor())
  {
#ifdef MESH_3_PROFILING
  WallClockTimer t;
#endif

#ifdef CGAL_LINKED_WITH_TBB
    //CJTODO TEST
    // Warning: this doesn't work for the becnhmark 
    // (because the benchmark creates the scheduler instance upstream...)
    //tbb::task_scheduler_init tsi(1);
#endif
  
    // Reset sliver value cache
    helper_.reset_cache();

    Mesh_optimization_return_code ret = 
      pump_vertices<true>(criterion_value_limit, visitor);
    
#ifdef MESH_3_PROFILING
  double exudation_time = t.elapsed();
  std::cerr << std::endl << "==== Total exudation 'wall-clock' time: " 
            << exudation_time << "s ====" << std::endl;
#endif
    return ret;
  }
  
  /// Time accessors
  void set_time_limit(double time) { time_limit_ = time; }
  double time_limit() const { return time_limit_; }
  
private:
  // -----------------------------------
  // Private Methods
  // -----------------------------------
  /**
   * Pumps vertices
   */
  template <bool pump_vertices_on_surfaces>
  Mesh_optimization_return_code
  pump_vertices(double criterion_value_limit, Visitor& v);
  
  /**
   * Pump one vertex
   */
  template <bool pump_vertices_on_surfaces>
  bool pump_vertex(const Vertex_handle& v,
                   bool *p_could_lock_zone = 0);
  
  /**
   * Returns the best_weight of v
   */
  double get_best_weight(const Vertex_handle& v,
                         bool *p_could_lock_zone = 0) const;
  
  /**
   * Initializes pre_star and criterion_values
   */
  void
  initialize_prestar_and_criterion_values(const Vertex_handle& v,
                                          Pre_star& pre_star,
                                          Sliver_values& criterion_values,
                                          bool *p_could_lock_zone = 0) const;
  
  /**
   * Expand pre_star with cell_to_add
   */
  bool expand_prestar(const Cell_handle& cell_to_add,
                      const Vertex_handle& pumped_vertex,
                      Pre_star& pre_star,
                      Sliver_values& criterion_values) const;
  
  /**
   * Returns Ordered_edge of facet which do not contains vertex
   */
  Ordered_edge get_opposite_ordered_edge(const Facet& facet,
                                         const Vertex_handle& vertex) const;
  
  /**
   * Returns the umbrella of internal_facets vector
   */
  Umbrella get_umbrella(const Facet_vector& internal_facets,
                        const Vertex_handle& v) const;
  
  /**
   * Updates the mesh with new_point
   */
  template <bool pump_vertices_on_surfaces>
  void update_mesh(const Weighted_point& new_point,
                   const Vertex_handle& old_vertex,
                   bool *p_could_lock_zone = 0);
  
  /**
   * Restores cells and boundary facets of conflict zone of new_vertex in c3t3_
   */
  template <bool pump_vertices_on_surfaces>
  void restore_cells_and_boundary_facets(
    const Boundary_facets_from_outside& boundary_facets_from_outside,
    const Vertex_handle& new_vertex);
  
  /**
   * Restore internal facets of conflict zone of new_vertex in c3t3_
   */
  void restore_internal_facets(const Umbrella& umbrella,
                               const Vertex_handle& new_vertex);
  
  /**
   * Orders handles \c h1 & \c h2
   */ 
  template <typename Handle>
  static
  void order_two_handles(Handle& h1, Handle& h2)
  {
    if( h2 < h1 )
      std::swap(h1, h2);
  }
  
  /**
   * Initialization
   */
  void init(double radius_ratio_limit = SliverCriteria::default_value )
  {
    if ( 0 < radius_ratio_limit )
      sliver_bound_ = radius_ratio_limit;
    else
      sliver_bound_ = SliverCriteria::max_value;
      
    cells_queue_.clear();
    initialize_cells_priority_queue();
    initialized_ = true;
  }
  
  /**
   * Initialize cells_queue w.r.t sliver_bound_
   */
  void initialize_cells_priority_queue()
  {
    for( Cell_iterator cit = c3t3_.cells_in_complex_begin() ;
        cit != c3t3_.cells_in_complex_end() ;
        ++cit)
    {
      const double value = sliver_criteria_(tr_.tetrahedron(cit));
      
      if( value < sliver_bound_ )
        cells_queue_.insert(cit, value);
    }
  }
  
  /**
   * Returns critical radius of (v,c)
   */
  double compute_critical_radius(const Vertex_handle& v,
                                 const Cell_handle& c) const
  {
    typedef typename Geom_traits::Compute_critical_squared_radius_3
      Critical_radius;
    
    Critical_radius critical_radius =
      tr_.geom_traits().compute_critical_squared_radius_3_object();
    
    return CGAL::to_double(critical_radius(c->vertex(0)->point(),
                                           c->vertex(1)->point(),
                                           c->vertex(2)->point(),
                                           c->vertex(3)->point(),
                                           v->point()));
  }
  
  /**
   * Returns the squared distance from vh to its closest vertice
   */
  double get_closest_vertice_squared_distance(const Vertex_handle& vh) const
  {

    double dist = (std::numeric_limits<double>::max)();
    details::Min_distance_from_v<Geom_traits, Vertex_handle>
      min_distance_from_v(vh, dist);

    tr_.adjacent_vertices(vh, boost::make_function_output_iterator(min_distance_from_v));
    
    return dist;
  }
  
  /** 
   * Returns the min value of second element of Ratio
   */
  double get_min_value(const Sliver_values& criterion_values) const
  {
    using boost::make_transform_iterator;
    typedef details::Second_of<Sliver_values> Second_of;
    
    return *(std::min_element(
      make_transform_iterator(criterion_values.begin(), Second_of()),
      make_transform_iterator(criterion_values.end(), Second_of())));
  }
  
  /**
   * Returns the \c Boundary_facets_from_outside object containing mirror facets
   * of \c facets
   */
  Boundary_facets_from_outside
  get_boundary_facets_from_outside(const Facet_vector& facets) const
  {
    Boundary_facets_from_outside boundary_facets_from_outside;
    
    for(typename Facet_vector::const_iterator fit = facets.begin();
        fit != facets.end();
        ++fit)
    {
      boundary_facets_from_outside.insert(std::make_pair(
        tr_.mirror_facet(*fit),
        std::make_pair(c3t3_.surface_patch_index(*fit),
                       c3t3_.subdomain_index(fit->first))));
    }
    
    return boundary_facets_from_outside;
  }
  
  /**
   * Add a cell \c ch to \c cells_queue
   */
  template <bool pump_vertices_on_surfaces>
  void add_cell_to_queue(Cell_handle ch, double criterion_value)
  {
#ifdef CGAL_LINKED_WITH_TBB
    // Parallel
    if (boost::is_base_of<Parallel_tag, Concurrency_tag>::value)
      enqueue_task<pump_vertices_on_surfaces>(ch, criterion_value);
    // Sequential
    else
#endif
      cells_queue_.insert(ch, criterion_value);
    
  }

  /**
   * Delete cells of \c cells from \c cells_queue
   */
  void delete_cells_from_queue(const Cell_vector& cells)
  {
    std::for_each(cells.begin(), cells.end(), 
                  Base::Erase_from_queue<Tet_priority_queue>(cells_queue_));
  }
  
  /**
   * A functor to remove one handle (Cell_handle/Facet_handle) from complex
   */
  class Remove_from_complex
  {
  public:
    Remove_from_complex(C3T3& c3t3)
    : c3t3_(c3t3) { }
    
    template <typename Handle_>
    void operator()(const Handle_& handle)
    { c3t3_.remove_from_complex(handle); }
    
  private:
    C3T3& c3t3_;
  };
  
  /**
   * Removes objects of [begin,end[ range from \c c3t3_
   */
  template<typename ForwardIterator>
  void remove_from_c3t3(ForwardIterator begin, ForwardIterator end)
  {
    std::for_each(begin, end, Remove_from_complex(c3t3_));
  }
  
  /**
   * Returns true if time_limit is reached
   */
  bool is_time_limit_reached() const
  {
    return ( (time_limit() > 0) && (running_time_.time() > time_limit()) );      
  }
  
  /**
   * Returns true if all cells of mesh have a sliver_criteria_ value greater
   * than sliver_bound_
   */
  bool check_sliver_bound() const
  {
    for( Cell_iterator cit = c3t3_.cells_in_complex_begin() ;
        cit != c3t3_.cells_in_complex_end() ;
        ++cit)
    {
      const double value = sliver_criteria_(tr_.tetrahedron(cit));
      
      if( value < sliver_bound_ )
        return false;
    }
    
    return true;
  }
  
#ifdef CGAL_LINKED_WITH_TBB
  // For parallel version
  template <bool pump_vertices_on_surfaces>
  void
  enqueue_task(Cell_handle ch, double value);
#endif

private:
  // -----------------------------------
  // Private data
  // -----------------------------------
  C3T3& c3t3_;
  Tr& tr_;
  double sq_delta_;
  double sliver_bound_;
  
  int num_of_pumped_vertices_;
  int num_of_ignored_vertices_;
  int num_of_treated_vertices_;
  
  bool initialized_;
  SliverCriteria sliver_criteria_;
  Tet_priority_queue cells_queue_;
  C3T3_helpers helper_;
  
  // Timer
  double time_limit_;
  CGAL::Timer running_time_;
  
#ifdef CGAL_LINKED_WITH_TBB
  // CJTOTO: change this for a better solution
  tbb::atomic<bool> m_lets_start_the_tasks;
#endif
  
#ifdef CGAL_MESH_3_DEBUG_SLIVERS_EXUDER
  // -----------------------------------
  // Debug Helpers
  // -----------------------------------
private:
  /**
   * Verifies that two doubles are near equal
   */
  static bool near_equal(const double& d1, const double& d2)
  {
    const double epsilon = 1e-8;
    return ( ((d1-d2) >= -1*epsilon) && ((d1-d2) <= epsilon) ); 
  }
  
  /**
   * Prints a double
   */
  static void print_double(const double& d)
  {
    std::cerr << d << " ; ";
  }
  
  /** This function verifies that the pre_star contains exactly the set of
   facets given by the sequence [begin, end[.
   
   If v!=0, it also fills another Pre_star object, from the sequence [begin,
   end[, and checks that is in the same order as pre_star.
   */
  template <class Input_facet_it>
  bool check_pre_star(const Pre_star& pre_star,
                      Input_facet_it begin,
                      Input_facet_it end,
                      const Vertex_handle v = Vertex_handle()) const;
  
  /** This function verifies that the pre_star contains exactly the set of
   facets on the boundary of the conflict zone of the weighted point wp.
   The vertex handle vh is an hint for the location of wp.
   
   It also fills another Pre_star object, and checks that is in the same
   order as pre_star.
   */
  bool check_pre_star(const Pre_star& pre_star,
                      const Weighted_point& wp,
                      const Vertex_handle& vh) const;
  
  /**
   * Checks if the sliver criterion values from \c criterion_values are the same as
   * those that will be found if wp is inserted in the triangulation
   */
  bool check_ratios(const Sliver_values& criterion_values,
                    const Weighted_point& wp,
                    const Vertex_handle& vh) const;

#endif // CGAL_MESH_3_DEBUG_SLIVERS_EXUDER
  
}; // end class Slivers_exuder

  
  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
Slivers_exuder<C3T3,Md,SC,V_,FT>::
Slivers_exuder(C3T3& c3t3, const Md& domain, const SC& criteria, double d)
  : Base(c3t3.bbox(),
         Concurrent_mesher_config::get().locking_grid_num_cells_per_axis)
  , c3t3_(c3t3)
  , tr_(c3t3_.triangulation())
  , sq_delta_(d*d)
  , sliver_bound_(0)
  , num_of_pumped_vertices_(0)
  , num_of_ignored_vertices_(0)
  , num_of_treated_vertices_(0)
  , initialized_(false)
  , sliver_criteria_(criteria)
  , helper_(c3t3_,domain,get_lock_data_structure())
  , time_limit_(-1)
  , running_time_()
{
  // If we're multi-thread
  tr_.set_lock_data_structure(get_lock_data_structure());
}  
  

template <typename C3T3, typename Md, typename SC, typename V_, typename FT>  
template <bool pump_vertices_on_surfaces>
Mesh_optimization_return_code
Slivers_exuder<C3T3,Md,SC,V_,FT>:: 
pump_vertices(double sliver_criterion_limit,
              Visitor& visitor)
{
#ifdef MESH_3_PROFILING
  WallClockTimer t;
#endif

  init(sliver_criterion_limit);

#ifdef MESH_3_PROFILING
  std::cerr << std::endl << "==== Init time: " 
            << t.elapsed() << "s ====" << std::endl;
#endif

#ifdef CGAL_MESH_3_EXUDER_VERBOSE
  std::cerr << "Exuding...\n";
  std::cerr << "Legend of the following line: "
            << "(#cells left,#vertices pumped,#vertices ignored)" << std::endl;

  std::cerr << "(" << cells_queue_.size() << ",0,0)";
#endif // CGAL_MESH_3_EXUDER_VERBOSE
  
  running_time_.reset();
  running_time_.start();
  
#ifdef MESH_3_PROFILING
  t.reset();
#endif

  
#ifdef CGAL_LINKED_WITH_TBB
  // Parallel
  if (boost::is_base_of<Parallel_tag, Concurrency_tag>::value)
  {
    m_lets_start_the_tasks = false;
    this->create_root_task();
    
    while (!cells_queue_.empty())
    {
      typename Tet_priority_queue::Reverse_entry front = *(cells_queue_.front());
      Cell_handle c = front.second;
      cells_queue_.pop_front();
      enqueue_task<pump_vertices_on_surfaces>(front.second, front.first);
    }

    m_lets_start_the_tasks = true;

    this->wait_for_all();

    std::cerr << " Flushing";
    bool keep_flushing = true;
    while (keep_flushing)
    {
      keep_flushing = this->flush_work_buffers();
      std::cerr << ".";
    }

    this->destroy_root_task();
  }
  // Sequential
  else
#endif // CGAL_LINKED_WITH_TBB
  {
    while( !cells_queue_.empty() && !is_time_limit_reached() )
    {
      typename Tet_priority_queue::Reverse_entry front = *(cells_queue_.front());
      Cell_handle c = front.second;
    
      bool vertex_pumped = false;
      for( int i = 0; i < 4; ++i )
      {
        // pump_vertices_on_surfaces is a boolean template parameter.  The
        // following condition is pruned at compiled time, if
        // pump_vertices_on_surfaces==false.
        if( pump_vertices_on_surfaces || c3t3_.in_dimension(c->vertex(i)) > 2 )
        {
          if( pump_vertex<pump_vertices_on_surfaces>(c->vertex(i)) )
          {
            vertex_pumped = true;
            ++num_of_pumped_vertices_;
            break;
          }
          else
            ++num_of_ignored_vertices_;
        
          ++num_of_treated_vertices_;
        }
      }
    
      // if the tet could not be deleted
      if ( ! vertex_pumped )
        cells_queue_.pop_front();

      visitor.after_cell_pumped(cells_queue_.size());
  #ifdef CGAL_MESH_3_EXUDER_VERBOSE   
      std::cerr << boost::format("\r             \r"
                                 "(%1%,%2%,%3%) (%|4$.1f| vertices/s)")
        % cells_queue_.size()
        % num_of_pumped_vertices_
        % num_of_ignored_vertices_
        % (num_of_treated_vertices_ / running_time_.time());
  #endif // CGAL_MESH_3_EXUDER_VERBOSE
    }
  }
  
  running_time_.stop();
  
#ifdef MESH_3_PROFILING
  std::cerr << std::endl << "==== Iterations time: " 
            << t.elapsed() << "s ====" << std::endl;
#endif

  
#ifdef CGAL_MESH_3_EXUDER_VERBOSE   
  std::cerr << std::endl;
  std::cerr << "Total exuding time: " << running_time_.time() << "s";
  std::cerr << std::endl;
#endif // CGAL_MESH_3_EXUDER_VERBOSE  
  
#ifdef CGAL_MESH_3_EXPORT_PERFORMANCE_DATA
  CGAL_MESH_3_SET_PERFORMANCE_DATA("Exuder_optim_time", running_time_.time());
#endif

  if ( is_time_limit_reached() ) {
#ifdef CGAL_MESH_3_EXUDER_VERBOSE
    std::cerr << "Exuding return code: TIME_LIMIT_REACHED\n\n";
#endif // CGAL_MESH_3_EXUDER_VERBOSE
    return TIME_LIMIT_REACHED;
  }
  
  if ( check_sliver_bound() ) {
#ifdef CGAL_MESH_3_EXUDER_VERBOSE
    std::cerr << "Exuding return code: BOUND_REACHED\n\n";
#endif // CGAL_MESH_3_EXUDER_VERBOSE
    return BOUND_REACHED;
  }
  
#ifdef CGAL_MESH_3_EXUDER_VERBOSE
    std::cerr << "Exuding return code: CANT_IMPROVE_ANYMORE\n\n";
#endif // CGAL_MESH_3_EXUDER_VERBOSE
  return CANT_IMPROVE_ANYMORE;
  
} // end function pump_vertices


template <typename C3T3, typename Md, typename SC, typename V_, typename FT>
template <bool pump_vertices_on_surfaces>
bool
Slivers_exuder<C3T3,Md,SC,V_,FT>::
pump_vertex(const Vertex_handle& pumped_vertex,
            bool *p_could_lock_zone)
{
  // Get best_weight
  double best_weight = get_best_weight(pumped_vertex, p_could_lock_zone);
  if (p_could_lock_zone && *p_could_lock_zone == false)
    return false;
  
  // If best_weight < pumped_vertex weight, nothing to do
  if ( best_weight > pumped_vertex->point().weight() )
  {
    Weighted_point wp(pumped_vertex->point(), best_weight);
    
    // Insert weighted point into mesh
    update_mesh<pump_vertices_on_surfaces>(wp, pumped_vertex, p_could_lock_zone);
    
    return true;
  }
  
  return false;
}
  
  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
void
Slivers_exuder<C3T3,Md,SC,V_,FT>::
initialize_prestar_and_criterion_values(const Vertex_handle& v,
                                        Pre_star& pre_star,
                                        Sliver_values& criterion_values,
                                        bool *p_could_lock_zone) const
{
  std::vector<Cell_handle> incident_cells;
  incident_cells.reserve(64);
  // Parallel
  if (p_could_lock_zone)
  {
    if (!helper_.try_lock_and_get_incident_cells(v, incident_cells))
    {
      *p_could_lock_zone = false;
      return;
    }
  }
  // Sequential
  else
  {
    tr_.incident_cells(v, std::back_inserter(incident_cells));
  }

  for ( typename Cell_vector::const_iterator cit = incident_cells.begin() ;
       cit != incident_cells.end() ;
       ++cit )
  {
    const int index = (*cit)->index(v);
    const Facet f = Facet(*cit, index);
    const Facet opposite_facet = tr_.mirror_facet(f);
    
    // Sliver criterion values initialization
    if( c3t3_.is_in_complex(*cit) )
    {
      double r = sliver_criteria_(tr_.tetrahedron(*cit));
      criterion_values[f] = r;
    }
    

    // Pre_star initialization
    // If facet is adjacent to and infinite cell, no need to put it in prestar
    // (infinite critical radius)
    if ( tr_.is_infinite(opposite_facet.first) )
      continue;
    
    // Insert facet in prestar (even if it is not in complex)
    double critical_radius = compute_critical_radius(v, opposite_facet.first);
    pre_star.insert(f, critical_radius);          
  }
}
  
  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
bool
Slivers_exuder<C3T3,Md,SC,V_,FT>::
expand_prestar(const Cell_handle& cell_to_add,
               const Vertex_handle& pumped_vertex,
               Pre_star& pre_star,
               Sliver_values& criterion_values) const
{
  // Delete first facet of pre_star
  Facet start_facet = pre_star.front()->second;
  CGAL_assertion(tr_.mirror_facet(start_facet).first == cell_to_add);
#ifdef CGAL_MESH_3_DEBUG_SLIVERS_EXUDER
  double critical_radius = pre_star.front()->first;
#endif
  pre_star.pop_front();
  if ( c3t3_.is_in_complex(cell_to_add) )
  {
    criterion_values.erase(start_facet);    
  }
  
  int start_mirror_facet_index = tr_.mirror_facet(start_facet).second;
  
  // For each facet of cell_to_add
  for(int i = 0; i<4 ; ++i)
  {
    // We have already treated start_facet
    if ( i == start_mirror_facet_index )
      continue;
    
    const Facet current_facet(cell_to_add, i);
    const Facet current_mirror_facet(tr_.mirror_facet(current_facet));
    
    // If current_facet_mirror is in prestar, delete it
    // (it may happen than pre_star contains two facets of the same cell)
    if ( pre_star.erase(current_mirror_facet) )
    {
      // If it is a boundary facet, stop pre_star expansion
      if ( c3t3_.is_in_complex(current_mirror_facet) )
      {
        return false;
      }
      
      // Update criterion_values
      if ( c3t3_.is_in_complex(cell_to_add) )
      { 
        criterion_values.erase(current_mirror_facet);
      }
    }
    // If current_mirror_facet is not in prestar:
    // expand prestar & update criterion_values 
    else 
    {
      const Cell_handle& current_mirror_cell = current_mirror_facet.first;
      
      CGAL_assertion(current_mirror_cell != start_facet.first);
      CGAL_assertion(pumped_vertex != current_mirror_facet.first->vertex(0));
      CGAL_assertion(pumped_vertex != current_mirror_facet.first->vertex(1));
      CGAL_assertion(pumped_vertex != current_mirror_facet.first->vertex(2));
      CGAL_assertion(pumped_vertex != current_mirror_facet.first->vertex(3));
      
      // Update pre_star (we do not insert facets with infinite critical radius)
      // We do insert facet of cells which are outside the complex (we just 
      // don't use their sliver criterion value to get best weight)
      if ( ! tr_.is_infinite(current_mirror_cell) )
      {
        double new_critical_radius = 
          compute_critical_radius(pumped_vertex, current_mirror_cell);

        pre_star.insert(current_facet, new_critical_radius);

#ifdef CGAL_MESH_3_DEBUG_SLIVERS_EXUDER     
        if ( new_critical_radius < critical_radius )
          std::cerr << "new critical radius:" << new_critical_radius
                    << " / current critical radius:" << critical_radius
                    << std::endl; 
#endif // CGAL_MESH_3_DEBUG_SLIVERS_EXUDER
      }
      
      // Update ratio (ratio is needed for cells of complex only)
      if ( c3t3_.is_in_complex(cell_to_add) )
      { 
        Tetrahedron_3 tet(pumped_vertex->point(),
                          cell_to_add->vertex((i+1)&3)->point(),
                          cell_to_add->vertex((i+2)&3)->point(),
                          cell_to_add->vertex((i+3)&3)->point());
        
        double new_value = sliver_criteria_(tet);
        criterion_values.insert(std::make_pair(current_facet,new_value));
      }
    }
  }        
  
  return true;
}

  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
double
Slivers_exuder<C3T3,Md,SC,V_,FT>::
get_best_weight(const Vertex_handle& v, bool *p_could_lock_zone) const
{
  // Get pre_star and criterion_values
  Pre_star pre_star;
  Sliver_values criterion_values;
  initialize_prestar_and_criterion_values(
    v, pre_star, criterion_values, p_could_lock_zone);

  if (p_could_lock_zone && *p_could_lock_zone == false)
    return 0.;

#ifdef CGAL_MESH_3_DEBUG_SLIVERS_EXUDER        
  Pre_star pre_star_copy;
  Sliver_values ratios_copy;
#endif
  
  double worst_criterion_value = get_min_value(criterion_values);
  double best_weight = 0;
  // CJTODO: this computes the incident cells again!
  double sq_d_v = get_closest_vertice_squared_distance(v);
  
  // If that boolean is set to false, it means that a facet in the complex
  // is about to be flipped. In that case, the pumping is stopped.
  bool can_flip = true;
  
  // Main loop: find the weight which maximizes the minimum value of ratio
  while(   can_flip
        && ! pre_star.empty()
        && pre_star.front()->first < (sq_delta_ * sq_d_v)
        && ! c3t3_.is_in_complex(pre_star.front()->second) )
  {
    // Store critial radius (pre_star will be modified in expand_prestar)
    double critical_r = pre_star.front()->first;
    
    // expand prestar (insert opposite_cell facets in pre_star)    
    Facet link = pre_star.front()->second;
    const Cell_handle& opposite_cell = tr_.mirror_facet(link).first;
    // CJTODO: useless?
    if (p_could_lock_zone && !tr_.try_lock_cell(opposite_cell))
    {
      *p_could_lock_zone = false;
      return 0.;
    }
    can_flip = expand_prestar(opposite_cell, v, pre_star, criterion_values);
    
    // Update best_weight if needed
    if(can_flip)
    {
      double min_of_pre_star = get_min_value(criterion_values);
      
      if( min_of_pre_star > worst_criterion_value )
      {
        // Update worst_criterion_value
        worst_criterion_value = min_of_pre_star;
        
        // Update best_weight
        CGAL_assertion(!pre_star.empty());
        double next_r = pre_star.front()->first;
        best_weight = (critical_r + next_r) / 2;
        
#ifdef CGAL_MESH_3_DEBUG_SLIVERS_EXUDER        
        pre_star_copy = pre_star;
        ratios_copy = criterion_values;
#endif // CGAL_MESH_3_DEBUG_SLIVERS_EXUDER  
      }
    }
  } // end while(... can pump...)
  
  
#ifdef CGAL_MESH_3_DEBUG_SLIVERS_EXUDER        
  if ( best_weight > v->point().weight() )
  {
    Weighted_point wp(v->point(), best_weight);
    check_pre_star(pre_star_copy, wp, v);
    check_ratios(ratios_copy, wp, v);
  }
#endif // CGAL_MESH_3_DEBUG_SLIVERS_EXUDER    
  
  return best_weight;
}
  

template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
typename Slivers_exuder<C3T3,Md,SC,V_,FT>::Umbrella
Slivers_exuder<C3T3,Md,SC,V_,FT>::
get_umbrella(const Facet_vector& facets,
             const Vertex_handle& v) const
{
  Umbrella umbrella;
  
  // Insert into umbrella surface_index of facets which are on the surface
  typename Facet_vector::const_iterator fit = facets.begin();
  for ( ; fit != facets.end() ; ++fit )
  {
    if ( c3t3_.is_in_complex(*fit) )
    {
      Ordered_edge edge = get_opposite_ordered_edge(*fit, v);
      umbrella.insert(std::make_pair(edge, c3t3_.surface_patch_index(*fit)));
    }
  }
  
  return umbrella;
}


template <typename C3T3, typename Md, typename SC, typename V_, typename FT>
template <bool pump_vertices_on_surfaces>
void
Slivers_exuder<C3T3,Md,SC,V_,FT>::  
restore_cells_and_boundary_facets(
  const Boundary_facets_from_outside& boundary_facets_from_outside,
  const Vertex_handle& new_vertex)
{
  Cell_vector new_cells;
  new_cells.reserve(64);
  tr_.incident_cells(new_vertex, std::back_inserter(new_cells));

  // Each cell must have a facet on the boundary of the conflict zone
  CGAL_assertion(boundary_facets_from_outside.size() == new_cells.size());
  
  // Restore attributes of each cell
  for(typename Cell_vector::iterator cit = new_cells.begin();
      cit != new_cells.end();
      ++cit)
  {
    (*cit)->invalidate_circumcenter();
    const int index = (*cit)->index(new_vertex);
    const Facet new_facet = std::make_pair(*cit, index);
    const Facet new_facet_from_outside = tr_.mirror_facet(new_facet);
    
    // Search new_facet_from_outside in boundary_facets_from_outside.
    // That search cannot fail.
    typename Boundary_facets_from_outside::const_iterator it =
      boundary_facets_from_outside.find(new_facet_from_outside);
    
    CGAL_assertion(it != boundary_facets_from_outside.end());
    
    // Restore facet attributes
    if ( it->second.first != Surface_patch_index() )
      c3t3_.add_to_complex(new_facet, it->second.first);
    
    // Restore cell attributes
    if ( it->second.second != Subdomain_index() )
      c3t3_.add_to_complex(*cit, it->second.second);
    
    // if the new cell is in the domain, and it criterion value is less that
    // the maximum, push it in the cells queue.
    if( c3t3_.is_in_complex(*cit) )
    {
      double criterion_value = sliver_criteria_(tr_.tetrahedron(*cit));

      if( criterion_value < sliver_bound_ )
        add_cell_to_queue<pump_vertices_on_surfaces>(*cit, criterion_value);
    }
  }  
}
  

  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
typename Slivers_exuder<C3T3,Md,SC,V_,FT>::Ordered_edge
Slivers_exuder<C3T3,Md,SC,V_,FT>::get_opposite_ordered_edge(
  const Facet& facet,
  const Vertex_handle& vertex) const
{
  Vertex_handle v1;
  Vertex_handle v2;
  
  // Get the two vertex of *fit which are not new_vertex
  for ( int i = 0 ; i < 4 ; ++i )
  {
    const Vertex_handle current_vertex = facet.first->vertex(i);
    
    if ( current_vertex != vertex && tr_.has_vertex(facet, current_vertex) )
    {
      if ( v1 == Vertex_handle() )
        v1 = current_vertex;
      else
        v2 = current_vertex;
    }
  }
  
  CGAL_assertion(v1 != Vertex_handle() && v2 != Vertex_handle());
  
  order_two_handles(v1,v2);
  return Ordered_edge(v1,v2);
}
  
  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
void
Slivers_exuder<C3T3,Md,SC,V_,FT>::  
restore_internal_facets(const Umbrella& umbrella,
                        const Vertex_handle& new_vertex)
{
  Facet_vector new_internal_facets;
  new_internal_facets.reserve(64);
  tr_.incident_facets(new_vertex, std::back_inserter(new_internal_facets));
  
  // Restore attributes of each facet
  for(typename Facet_vector::iterator fit = new_internal_facets.begin();
      fit != new_internal_facets.end();
      ++fit)
  {
    Ordered_edge edge = get_opposite_ordered_edge(*fit, new_vertex);

    // Search edge in umbrella.
    // If it is found, restore facet surface index from umbrella
    const typename Umbrella::const_iterator um_it = umbrella.find(edge);
    if( um_it != umbrella.end() )
    {
      c3t3_.add_to_complex(*fit, um_it->second);
    }
  }
}

  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT>
template <bool pump_vertices_on_surfaces>
void
Slivers_exuder<C3T3,Md,SC,V_,FT>::  
update_mesh(const Weighted_point& new_point,
            const Vertex_handle& old_vertex,
            bool *p_could_lock_zone)
{
  CGAL_assertion_code(std::size_t nb_vert = 
                      tr_.number_of_vertices());
  Cell_vector deleted_cells;
  Facet_vector internal_facets;
  Facet_vector boundary_facets;
  
  deleted_cells.reserve(64);
  internal_facets.reserve(64);
  boundary_facets.reserve(64);
  
  tr_.find_conflicts(new_point,
                     old_vertex->cell(),
                     std::back_inserter(boundary_facets),
                     std::back_inserter(deleted_cells),
                     std::back_inserter(internal_facets),
                     p_could_lock_zone);
  
  if (p_could_lock_zone && *p_could_lock_zone == false)
    return;

  // Get some datas to restore mesh
  Boundary_facets_from_outside boundary_facets_from_outside =
    get_boundary_facets_from_outside(boundary_facets);
  
  Umbrella umbrella = get_umbrella(internal_facets, old_vertex);

  // Delete old cells from queue (they aren't in the triangulation anymore)
  delete_cells_from_queue(deleted_cells);
  
  // Delete old cells & facets from c3t3
  remove_from_c3t3(deleted_cells.begin(),deleted_cells.end());
  remove_from_c3t3(boundary_facets.begin(),boundary_facets.end());
  remove_from_c3t3(internal_facets.begin(),internal_facets.end());
  
  // Insert new point (v will be updated using a wp)
  int dimension = c3t3_.in_dimension(old_vertex);
  Index vertice_index = c3t3_.index(old_vertex);
  
  Vertex_handle new_vertex = tr_.insert(new_point, old_vertex->cell());
  c3t3_.set_dimension(new_vertex,dimension);
  c3t3_.set_index(new_vertex,vertice_index);

  // Only true for sequential version
  CGAL_assertion(p_could_lock_zone || nb_vert == tr_.number_of_vertices());

  // Restore mesh
  restore_cells_and_boundary_facets<pump_vertices_on_surfaces>(
    boundary_facets_from_outside, new_vertex);
  restore_internal_facets(umbrella, new_vertex);

  // Only true for sequential version
  CGAL_assertion(p_could_lock_zone || nb_vert == tr_.number_of_vertices());
}


#ifdef CGAL_LINKED_WITH_TBB
// For parallel version
template <typename C3T3, typename Md, typename SC, typename V_, typename FT>
template <bool pump_vertices_on_surfaces>
void
Slivers_exuder<C3T3,Md,SC,V_,FT>::
enqueue_task(Cell_handle ch, double value)
{
  int erase_counter = ch->get_erase_counter();
  enqueue_work(
    [&, ch, erase_counter]()
    {
#ifdef CGAL_CONCURRENT_MESH_3_PROFILING
      static Profile_branch_counter_3 bcounter(
        "early withdrawals / late withdrawals / successes [Exuder]");
#endif
      while (!m_lets_start_the_tasks)
        std::this_thread::yield();

      for( int i = 0; i < 4; ++i )
      {
        bool could_lock_zone;
        do
        {
          could_lock_zone = true;

          if (ch->get_erase_counter() != erase_counter)
            break;

          if (!tr_.try_lock_cell(ch))
          {
#ifdef CGAL_CONCURRENT_MESH_3_PROFILING
            bcounter.increment_branch_2(); // THIS is an early withdrawal!
#endif
            could_lock_zone = false;
            this->unlock_all_elements();
            continue;
          }

          if (ch->get_erase_counter() != erase_counter)
          {
            this->unlock_all_elements();
            break;
          }

          // pump_vertices_on_surfaces is a boolean template parameter.  The
          // following condition is pruned at compiled time, if
          // pump_vertices_on_surfaces==false.
          if (pump_vertices_on_surfaces || c3t3_.in_dimension(ch->vertex(i)) > 2)
          {
            this->pump_vertex<pump_vertices_on_surfaces>(
              ch->vertex(i), &could_lock_zone);
            
#ifdef CGAL_CONCURRENT_MESH_3_PROFILING
            if (!could_lock_zone)
              bcounter.increment_branch_1(); // THIS is a late withdrawal!
            else
              ++bcounter; // Success!
#endif
          }
    
          this->unlock_all_elements();
        } while (!could_lock_zone);
      }
    },
    value);
}
#endif

  
#ifdef CGAL_MESH_3_DEBUG_SLIVERS_EXUDER  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
template <class Input_facet_it>
bool
Slivers_exuder<C3T3,Md,SC,V_,FT>::
check_pre_star(const Pre_star& pre_star,
               Input_facet_it begin,
               Input_facet_it end,
               const Vertex_handle v) const
{
  Pre_star pre_star_copy = pre_star;
  if(v != Vertex_handle())
  {
    Pre_star pre_star2;
    
    // fill pre_star2
    for(Input_facet_it fit = begin;
        fit != end;
        ++fit)
    {
      const Facet opposite_facet = tr_.mirror_facet(*fit);
      if(! tr_.is_infinite(opposite_facet.first) )
      {
        pre_star2.insert(*fit, compute_critical_radius(v,
                                                       opposite_facet.first));
      }
    }
    
    while(!pre_star_copy.empty() && !pre_star2.empty())
    {
      if(pre_star_copy.front()->first != pre_star2.front()->first) {
        std::cerr << "bad order\n";
        std::cerr << boost::format("pre_star.front()->first=%1%, should be %2%\n")
        % pre_star_copy.front()->first % pre_star2.front()->first;
        return false;
      }
      if ( pre_star_copy.front()->second != pre_star2.front()->second ) 
      {
        Facet f1 = pre_star_copy.front()->second;
        Facet f2 = pre_star2.front()->second;
        pre_star2.pop_front();
        pre_star_copy.pop_front();
        if ( pre_star_copy.front()->second == f2 && pre_star2.front()->second == f1 )
        {
          // It's ok
          pre_star2.pop_front();
          pre_star_copy.pop_front();
        }
        else
        {
          Facet f1 = tr_.mirror_facet(pre_star_copy.front()->second);
          Facet f2 = tr_.mirror_facet(pre_star2.front()->second);
          std::cerr << "Bad facet:" << f1.second << "/" << f2.second
          << " - " << &*f1.first << "/" << &*f2.first << std::endl;
        }
      }
      else
      {
        pre_star2.pop_front();
        pre_star_copy.pop_front();
      }
    }
    
    if(pre_star2.empty() && ! pre_star_copy.empty()) {
      std::cerr << "pre_star is too big!\n";
      while(!pre_star_copy.empty())
      {
        const Facet f = pre_star_copy.front()->second;
        const double r = pre_star_copy.front()->first;
        pre_star_copy.pop_front();
        std::cerr << boost::format("extra facet (%1%,%2%) (infinite: %3%, opposite infinite: %4%), critical radius: %5%\n")
        % &*f.first % f.second % tr_.is_infinite(f.first) % tr_.is_infinite(f.first->neighbor(f.second))
        % r;
      }
      return false;
    }
    
    if( pre_star_copy.empty() && ! pre_star2.empty() ) {
      std::cerr << "pre_star is too small!\n";
      while(!pre_star2.empty())
      {
        const Facet f = pre_star2.front()->second;
        pre_star2.pop_front();
        std::cerr << boost::format("missing facet (%1%,%2%) (infinite: %3%, opposite infinite: %4%)\n")
        % &*f.first % f.second % tr_.is_infinite(f.first) % tr_.is_infinite(f.first->neighbor(f.second));
      }
      return false;
    }
  }
  
  pre_star_copy = pre_star;
  
  for(Input_facet_it fit = begin;
      fit != end;
      ++fit)
  {
    const Facet opposite_facet = tr_.mirror_facet(*fit);
    if(!tr_.is_infinite(opposite_facet.first) && !pre_star_copy.erase(*fit))
      return false;
  }
  if( !pre_star_copy.empty() )
    return false;
  
  return true;
}

  

template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
bool
Slivers_exuder<C3T3,Md,SC,V_,FT>::
check_pre_star(const Pre_star& pre_star,
               const Weighted_point& wp,
               const Vertex_handle& vh) const
{
  std::vector<Facet> boundary_facets;
  boundary_facets.reserve(64);
  
  tr_.find_conflicts(wp,
                     vh->cell(),
                     std::back_inserter(boundary_facets),
                     CGAL::Emptyset_iterator(),
                     CGAL::Emptyset_iterator());
  
  const bool result =  check_pre_star(pre_star,
                                      boundary_facets.begin(),
                                      boundary_facets.end(),
                                      vh);
  if( ! result )
    std::cerr << "boundary_facets.size()=" << boundary_facets.size()
    << "\npre_star.size()=" << pre_star.size()
    << "\ntested wp=" << wp
    << "\n";
  return result;
}

  
template <typename C3T3, typename Md, typename SC, typename V_, typename FT> 
bool
Slivers_exuder<C3T3,Md,SC,V_,FT>::
check_ratios(const Sliver_values& criterion_values,
                  const Weighted_point& wp,
                  const Vertex_handle& vh) const
{
  Cell_vector deleted_cells;
  Facet_vector internal_facets;
  Facet_vector boundary_facets;
  
  tr_.find_conflicts(wp,
                     vh->cell(),
                     std::back_inserter(boundary_facets),
                     std::back_inserter(deleted_cells),
                     std::back_inserter(internal_facets));
  
  bool result = true;
  std::vector<double> expected_ratios;
  std::vector<double> ratio_vector;
  
  for ( typename Sliver_values::const_iterator rit = criterion_values.begin() ;
       rit != criterion_values.end() ;
       ++rit )
  {
    ratio_vector.push_back(rit->second);
  }
  
  for ( typename Facet_vector::const_iterator it = boundary_facets.begin() ;
       it != boundary_facets.end() ;
       ++ it )
  {
    if ( !c3t3_.is_in_complex((it->first)) )
      continue;
    
    int k = it->second;
    Tetrahedron_3 tet(vh->point(),
                      it->first->vertex((k+1)&3)->point(),
                      it->first->vertex((k+2)&3)->point(),
                      it->first->vertex((k+3)&3)->point());
    
    double ratio = sliver_criteria_(tet);
    expected_ratios.push_back(ratio); 
    
    bool found = false;
    for ( typename Sliver_values::const_iterator rit = criterion_values.begin() ;
         rit != criterion_values.end() ;
         ++rit )
    {
      if ( near_equal(rit->second,ratio) )
      {
        found = true;
        break;
      }
    }
    if ( ! found )
    { 
      result = false;
    }
  }
  
  if (expected_ratios.size() != criterion_values.size())
    result = false;
  
  if ( !result )
  {
    std::sort(expected_ratios.begin(),expected_ratios.end());
    std::sort(ratio_vector.begin(),ratio_vector.end());
    std::vector<double> diff;
    std::set_difference(expected_ratios.begin(),expected_ratios.end(),
                        ratio_vector.begin(),ratio_vector.end(),
                        std::back_inserter(diff));
    
    
    std::cerr << "\nExpected criterion_values:[";
    std::for_each(expected_ratios.begin(), expected_ratios.end(), print_double);
    std::cerr << "]\nRatios:[";
    std::for_each(ratio_vector.begin(), ratio_vector.end(), print_double);
    std::cerr << "]\nDiff:[";
    std::for_each(diff.begin(),diff.end(), print_double);
    std::cerr << "]\n";
  }
  
  return result;
}
#endif // CGAL_MESH_3_DEBUG_SLIVERS_EXUDER
  

} // end namespace Mesh_3

} // end namespace CGAL


#endif // end CGAL_MESH_3_SLIVERS_EXUDER_H
