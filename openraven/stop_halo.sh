#!/bin/bash
# Force-close Halo: Fireteam Raven (the arcade build has no quit option).
pkill -9 -f '\./game -f' && echo "Halo closed." || echo "Halo is not running."
