define show_fair_group
	set $peers = (ngx_http_upstream_fair_peers_t *)$arg0
	if ($peers == 0)
		printf "null fair group\n"
	else
		printf "upstream %s current %u/%u total_nreq %u total_requests %u\n", \
			$peers->name->data, $peers->current, $peers->number, \
			$peers->shared->total_nreq, $peers->shared->total_requests
		set $i = 0
		while $i < $peers->number
			set $peer = &$peers->peer[$i]
			printf "peer %u: %s weight %u/%u conns %u fails %u/%u nreq %u checked %ld\n", \
				$i, $peer->name.data, $peer->shared->current_weight, $peer->weight, \
				$peer->max_conns, $peer->shared->fails, $peer->max_fails, \
				$peer->shared->nreq, $peer->shared->checked
			set $i = $i + 1
		end
		printf "-----------------\n"
		if ($peers->next != 0)
			show_fair_group $peers->next
		end
	end
end
document show_fair_group
Dump one fair group and its backup group from a peers pointer.
end
