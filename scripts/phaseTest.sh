#!/bin/bash

# Region to run script on
region=$1
options=$2

# Record commands
set -o xtrace

# Run margin
echo Running Margin
./margin ../tests/shasta_phasing_data.100kb_5x/${region}/*.bam ../tests/shasta_phasing_data.100kb_5x/${region}/HG002.shasta.*.fasta ../params/allParams.np.json ${options}  --logLevel DEBUG

# Calculate identity
echo Comparing to haplotype1
python3 ../scripts/dirty_assembly_compare.py ../tests/shasta_phasing_data.100kb_5x/${region}/HG002_h1*.fa output.fa

echo Comparing to haplotype2
python3 ../scripts/dirty_assembly_compare.py ../tests/shasta_phasing_data.100kb_5x/${region}/HG002_h2*.fa output.fa

# Build collection of differences
python3 ../scripts/dirty_assembly_compare.py output.fa output.fa verbose > predictedMismatches.txt
python3 ../scripts/dirty_assembly_compare.py ../tests/shasta_phasing_data.100kb_5x/${region}/HG002_h1*.fa ../tests/shasta_phasing_data.100kb_5x/${region}/HG002_h2*.fa verbose > trueMismatches.txt

# Compare differences
echo Comparing predicted hets
python3 ../scripts/compareHets.py trueMismatches.txt predictedMismatches.txt

