/******************************************************************************
 * size_constraint_label_propagation.h     
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/


#include <unordered_map>

#include <sstream>
#include "../edge_rating/edge_ratings.h"
#include "../matching/gpa/gpa_matching.h"
#include "data_structure/union_find.h"
#include "node_ordering.h"
#include "tools/quality_metrics.h"
#include "tools/random_functions.h"
#include "io/graph_io.h"

#include "size_constraint_label_propagation.h"

size_constraint_label_propagation::size_constraint_label_propagation() {
                
}

size_constraint_label_propagation::~size_constraint_label_propagation() {
                
}

void size_constraint_label_propagation::match(const PartitionConfig & partition_config, 
                                              graph_access & G, 
                                              Matching & _matching, 
                                              CoarseMapping & coarse_mapping, 
                                              NodeID & no_of_coarse_vertices,
                                              NodePermutationMap & permutation) {
        permutation.resize(G.number_of_nodes());
        coarse_mapping.resize(G.number_of_nodes());
        no_of_coarse_vertices = 0;

        if ( partition_config.ensemble_clusterings ) {
                ensemble_clusterings(partition_config, G, _matching, coarse_mapping, no_of_coarse_vertices, permutation);
        } else {
                match_internal(partition_config, G, _matching, coarse_mapping, no_of_coarse_vertices, permutation);
        }
}

void size_constraint_label_propagation::match_internal(const PartitionConfig & partition_config, 
                                              graph_access & G, 
                                              Matching & _matching, 
                                              CoarseMapping & coarse_mapping, 
                                              NodeID & no_of_coarse_vertices,
                                              NodePermutationMap & permutation) {

        std::vector<NodeID> cluster_id(G.number_of_nodes());
        NodeWeight block_upperbound = ceil(partition_config.upper_bound_partition/(double)partition_config.cluster_coarsening_factor);

	label_propagation( partition_config, G, block_upperbound, cluster_id, no_of_coarse_vertices);

        create_coarsemapping( partition_config, G, cluster_id, coarse_mapping);
}

void size_constraint_label_propagation::ensemble_two_clusterings( graph_access & G, 
                                                                  std::vector<NodeID> & lhs, 
                                                                  std::vector<NodeID> & rhs, 
                                                                  std::vector< NodeID > & output,
                                                                  NodeID & no_of_coarse_vertices) {


        hash_ensemble new_mapping; 
        no_of_coarse_vertices = 0;
        for( NodeID node = 0; node < lhs.size(); node++) {
                ensemble_pair cur_pair;
                cur_pair.lhs = lhs[node]; 
                cur_pair.rhs = rhs[node]; 
                cur_pair.n   = G.number_of_nodes(); 

                if(new_mapping.find(cur_pair) == new_mapping.end() ) {
                        new_mapping[cur_pair].mapping = no_of_coarse_vertices;
                        no_of_coarse_vertices++;
                }

                output[node] = new_mapping[cur_pair].mapping;
        }

        no_of_coarse_vertices = new_mapping.size();
}


void size_constraint_label_propagation::ensemble_clusterings(const PartitionConfig & partition_config, 
                                                             graph_access & G, 
                                                             Matching & _matching, 
                                                             CoarseMapping & coarse_mapping, 
                                                             NodeID & no_of_coarse_vertices,
                                                             NodePermutationMap & permutation) {
        int runs = partition_config.number_of_clusterings;
        std::vector< NodeID >  cur_cluster(G.number_of_nodes(), 0);
        std::vector< NodeID >  ensemble_cluster(G.number_of_nodes(),0);

        int new_cf = partition_config.cluster_coarsening_factor;
        for( int i = 0; i < runs; i++) {
                PartitionConfig config = partition_config;
                config.cluster_coarsening_factor = new_cf;

                NodeID cur_no_blocks = 0;
                label_propagation(config, G, cur_cluster, cur_no_blocks); 

                if( i != 0 ) {
                        ensemble_two_clusterings(G, cur_cluster, ensemble_cluster, ensemble_cluster, no_of_coarse_vertices);
                } else {
                        forall_nodes(G, node) {
                                ensemble_cluster[node] = cur_cluster[node];
                        } endfor
                        
                        no_of_coarse_vertices = cur_no_blocks;
                }
                new_cf = random_functions::nextInt(10, 30);
        }

        create_coarsemapping( partition_config, G, ensemble_cluster, coarse_mapping);


}

void size_constraint_label_propagation::label_propagation(const PartitionConfig & partition_config, 
                                                         graph_access & G, 
                                                         std::vector<NodeID> & cluster_id, 
                                                         NodeID & no_of_blocks ) {
        NodeWeight block_upperbound = ceil(partition_config.upper_bound_partition/(double)partition_config.cluster_coarsening_factor);

        label_propagation( partition_config, G, block_upperbound, cluster_id, no_of_blocks);
}

void size_constraint_label_propagation::label_propagation(const PartitionConfig & partition_config, 
                                                         graph_access & G, 
                                                         const NodeWeight & block_upperbound,
                                                         std::vector<NodeID> & cluster_id,  
                                                         NodeID & no_of_blocks) {
        // in this case the _matching paramter is not used 
        // coarse_mappng stores cluster id and the mapping (it is identical)
	random_functions::fastRandBool<uint64_t> random_obj;
	NodeID moving_nodes = G.number_of_nodes() - partition_config.quotient_nodes;
        std::vector<EdgeWeight> hash_map(G.number_of_nodes(),0);
        std::vector<NodeID> permutation(moving_nodes);
        std::vector<NodeID> neighboring_blocks(moving_nodes);
        std::vector<NodeWeight> cluster_sizes(G.number_of_nodes());
        cluster_id.resize(G.number_of_nodes());
	std::vector<NodeID> isolated_nodes;

	forall_nodes(G, node) {
		cluster_sizes[node] = G.getNodeWeight(node);
		cluster_id[node]    = node;
	} endfor
        
        node_ordering n_ordering;
	n_ordering.order_nodes(partition_config, G, permutation);

        for( int j = 0; j < partition_config.label_iterations; j++) {
                [[maybe_unused]] unsigned int change_counter = 0;
		for (NodeID node: permutation) {
                        //now move the node to the cluster that is most common in the neighborhood
                        forall_out_edges(G, e, node) {
                                NodeID target = G.getEdgeTarget(e);
				if (target >= G.number_of_nodes() - partition_config.quotient_nodes ||
				   (partition_config.graph_allready_partitioned && G.getPartitionIndex(node) != G.getPartitionIndex(target)) ||
				   (partition_config.combine && G.getSecondPartitionIndex(node) != G.getSecondPartitionIndex(target))) {
					continue;
				}
                                PartitionID cur_block = cluster_id[target];
                                EdgeWeight  cur_value = hash_map[cur_block];
				if (cur_value == 0) {
					neighboring_blocks.push_back(cur_block);
				}
                                hash_map[cur_block] = cur_value + G.getEdgeWeight(e);
                        } endfor

			if (j == 0 && neighboring_blocks.size() == 0) {
				isolated_nodes.push_back(node);
			}

                        //second sweep for finding max and resetting array
                        PartitionID my_block  = cluster_id[node];
                        PartitionID max_block = my_block;

                        PartitionID max_value = 0;
			for (auto const & cur_block : neighboring_blocks) {
                                PartitionID cur_value     = hash_map[cur_block];
				if((cur_value > max_value  || (cur_value == max_value && random_obj.nextBool())) &&
						(cluster_sizes[cur_block] + G.getNodeWeight(node) < block_upperbound || cur_block == my_block)) {
					max_value = cur_value;
					max_block = cur_block;
				}

                                hash_map[cur_block] = 0;
                        } 

			neighboring_blocks.clear();

                        cluster_sizes[my_block]		 -= G.getNodeWeight(node);
                        cluster_sizes[max_block]         += G.getNodeWeight(node);
                        change_counter                   += (cluster_id[node] != max_block);
                        cluster_id[node]                  = max_block;
                }
        }

	if (isolated_nodes.size() >= 2) {
		cluster_isolated_nodes(partition_config, G, cluster_id, cluster_sizes, block_upperbound, isolated_nodes);
	}

//std::cout << "partition_config.remaining_stream_nodes: " << partition_config.remaining_stream_nodes << "\n";
        remap_cluster_ids( partition_config, G, cluster_id, no_of_blocks);
}


void size_constraint_label_propagation::create_coarsemapping(const PartitionConfig & partition_config, 
                                                             graph_access & G,
                                                             std::vector<NodeID> & cluster_id,
                                                             CoarseMapping & coarse_mapping) {
        forall_nodes(G, node) {
                coarse_mapping[node] = cluster_id[node];
        } endfor
}

void size_constraint_label_propagation::remap_cluster_ids(const PartitionConfig & partition_config, 
                                                          graph_access & G,
                                                          std::vector<NodeID> & cluster_id,
                                                          NodeID & no_of_coarse_vertices, bool apply_to_graph) {

        PartitionID cur_no_clusters = 0;
        std::unordered_map<PartitionID, PartitionID> remap;
        forall_nodes(G, node) {
                PartitionID cur_cluster = cluster_id[node];
                //check wether we already had that
                if( remap.find( cur_cluster ) == remap.end() ) {
                        remap[cur_cluster] = cur_no_clusters++;
                }

                cluster_id[node] = remap[cur_cluster];
        } endfor

        if( apply_to_graph ) {
                forall_nodes(G, node) {
                        G.setPartitionIndex(node, cluster_id[node]);
                } endfor
                G.set_partition_count(cur_no_clusters);
        }

        no_of_coarse_vertices = cur_no_clusters;
}


void size_constraint_label_propagation::cluster_isolated_nodes(const PartitionConfig & partition_config, 
                                                              graph_access & G,
                                                              std::vector<NodeID> & cluster_id,
							      std::vector<NodeWeight> & cluster_sizes,
                                                              const NodeWeight & block_upperbound,
                                                              std::vector<NodeID> & isolated_nodes) {
	std::vector<bool> combined(isolated_nodes.size(),false);
	NodeID node, target;
	PartitionID cur_block;

	for (NodeID it = 0; it < isolated_nodes.size(); it++) {
		node = isolated_nodes[it];
		if (combined[it]) {
			continue;
		}
		combined[it] = true;
		for (int it2 = it+1; it2 < isolated_nodes.size(); it2++) {
			target = isolated_nodes[it2];
			if (combined[it2]) {
				continue;
			}
			cur_block = cluster_id[target];
			if ( (cluster_sizes[cur_block] + G.getNodeWeight(node) <= block_upperbound || cur_block == cluster_id[node]) 
			&& (!partition_config.graph_allready_partitioned || G.getPartitionIndex(node) == G.getPartitionIndex(target))
			&& (!partition_config.combine || G.getSecondPartitionIndex(node) == G.getSecondPartitionIndex(target)))
			{
				combined[it2] = true;
				cluster_sizes[cluster_id[node]]  -= G.getNodeWeight(node);
				cluster_sizes[cur_block]         += G.getNodeWeight(node);
				cluster_id[node]                  = cur_block;
				break;
			}

		}
	}
}

