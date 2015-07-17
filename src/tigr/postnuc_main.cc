//------------------------------------------------------------------------------
//   Programmer: Adam M Phillippy, The Institute for Genomic Research
//               Guillaume Marcais, University of Maryland
//         File: postnuc.cc
//         Date: 07 / 16 / 2002
//
//     Revision: 08 / 01 / 2002
//               Added MUM extension functionality
//
//      Purpose: To translate the coordinates referencing the concatenated
//              reference sequences back to the individual sequences and deal
//             with any conflict that may have arisen (i.e. a MUM that spans
//            the boundry between two sequences). Then to extend each cluster
//           via Smith-Waterman techniques to expand the alignment coverage.
//          Alignments which encounter each other will be fused to form one
//         encompasing alignment when appropriate.
//
//        Input: Input is the output of the .mgaps program from stdin. On the
//              command line, the file names of the two original sequence files
//             should come first, followed by the prefix <pfx> that should be
//            placed in front of the two output filenames <pfx>.cluster and
//           <pfx>.delta
//
// NOTE: Cluster file is now suppressed by default (see -d option).
//
//       Output: Output is to two output files, <pfx>.cluster and <pfx>.delta.
//              <pfx>.cluster lists MUM clusters as identified by "mgaps".
//             However, the clusters now reference their corresponding
//            sequence and are all listed under headers that specify this
//           sequence. The <pfx>.delta file is the alignment object that
//          contains all the information necessary to reproduce the alignments
//         generated by the MUM extension process. Please refer to the
//        output file documentation for an in-depth description of these
//       file formats.
//
//        Usage: postnuc  <reference>  <query>  <pfx>  <  <input>
//           Where <reference> and <query> are the original input sequences of
//          NUCmer and <pfx> is the prefix that should be added to the
//         beginning of the <pfx>.cluster and <pfx>.delta output filenames.
//
//------------------------------------------------------------------------------

#include <iostream>
#include <fstream>
#include <algorithm>
#include "postnuc.hh"
#include "tigrinc.hh"
#include "sw_align.hh"

using namespace std;
using namespace mummer::postnuc;

//------------------------------------------------------ Globals -------------//
bool DO_DELTA   = true;
bool DO_EXTEND  = true;
bool TO_SEQEND  = false;
bool DO_SHADOWS = false;

void printHelp
(const char * s)

//  Display the program's help information to stderr.

{
  cerr << "\nUSAGE: " << s << " [options]  <reference>  <query>  <pfx>  <  <input>\n\n"
       << "-b int  set the alignment break (give-up) length to int\n"
       << "-B int  set the diagonal banding for extension to int\n"
       << "-d      output only match clusters rather than extended alignments\n"
       <<  "-e      do not extend alignments outward from clusters\n"
       <<  "-h      display help information\n"
       <<  "-s      don't remove shadowed alignments, useful for aligning a\n"
       <<  "        sequence to itself to identify repeats\n"
       <<  "-t      force alignment to ends of sequence if within -b distance\n\n"
       << "  Input is the output of the \"mgaps\" program from stdin, and\n"
       << "the two original NUCmer sequence files passed on the command\n"
       << "line. <pfx> is the prefix to be added to the front of the\n"
       <<  "output file <pfx>.delta\n"
       <<  "  <pfx>.delta is the alignment object that catalogs the distance\n"
       <<  "between insertions and deletions. For further information\n"
       <<  "regarding this file, please refer to the documentation under\n"
       <<  "the .delta output description.\n\n";
}




void printUsage
(const char * s)

//  Display the program's usage information to stderr.

{
  cerr << "\nUSAGE: " << s << " [options]  <reference>  <query>  <pfx>  <  <input>\n\n"
       << "Try '" << s << " -h' for more information.\n";
}

void parse_options(int argc, char* argv[]) {
  optarg = NULL;
  int ch, errflg = 0;
  while ( !errflg  &&  ((ch = getopt (argc, argv, "dehB:b:st")) != EOF) ) {
    switch (ch) {
    case 'b' :
      setBreakLen( atoi (optarg) );
      break;

    case 'B' :
      setBanding( atoi (optarg) );
      break;

    case 'd' :
      DO_DELTA = false;
      break;

    case 'e' :
      DO_EXTEND = false;
      break;

    case 'h' :
      printHelp (argv[0]);
      exit (EXIT_SUCCESS);

    case 's' :
      DO_SHADOWS = true;
      break;

    case 't' :
      TO_SEQEND = true;
      break;

    default :
      errflg ++;
    }
  }
  if ( errflg > 0 || argc - optind != 3 ) {
    printUsage (argv[0]);
    exit (EXIT_FAILURE);
  }
}

class FastaRecord
//-- The essential data of a sequence. 1-based record. First character
//-- of m_seq is always a '\0'. len() returns the length of the
//-- sequence (i.e. without counting the initial (or terminating)
//-- '\0'.
{
  std::string m_Id;               // the fasta ID header tag
  std::string m_seq;              // the sequence data

public:
  FastaRecord() = default;
  FastaRecord(const std::string& Id, const std::string& seq)
    : m_Id(Id)
    , m_seq(seq)
  { }
  FastaRecord(std::string&& Id, std::string&& seq)
    : m_Id(std::move(Id))
    , m_seq(std::move(seq))
  { }
  FastaRecord(FastaRecord&& rhs) noexcept : m_Id(std::move(rhs.m_Id)), m_seq(std::move(rhs.m_seq)) { }
  const std::string& Id() const { return m_Id; }
  long int len() const { return m_seq.size() - 1; }
  const char* seq() const { return m_seq.c_str(); }

  bool read_sequence(std::istream& is) {
    return Read_Sequence(is, m_seq, m_Id);
  }
};


int main(int argc, char *argv[]) {
  std::ios::sync_with_stdio(false);

  typedef Synteny<FastaRecord>      synteny_type;
  typedef std::vector<synteny_type> synteny_list_type;
  std::vector<FastaRecord>            Af; // array of all the reference sequences
  synteny_list_type                   Syntenys; // vector of all sets of clusters
  synteny_list_type::reverse_iterator CurrSp; // current set of clusters

  FastaRecord   Bf;             // Query sequence information
  string        Line;           // a single line of input
  string        CurrIdB;        // fasta ID headers
  const string* IdA;


  size_t   Seqi;                // current reference sequence index
  long int sA, sB, len;         // current match start in A, B and length

  //-- Set the alignment data type and break length (sw_align.h)
  setMatrixType ( NUCLEOTIDE );
  setBreakLen ( 200 );
  setBanding ( 0 );

  //-- Parse the command line arguments
  parse_options(argc, argv);

  merge_syntenys merger(DO_DELTA, DO_EXTEND, TO_SEQEND, DO_SHADOWS);

  //-- Read and create the I/O file names
  string RefFileName(argv[optind ++]);
  string QryFileName(argv[optind ++]);
  string ClusterFileName(argv[optind ++]);
  string DeltaFileName(ClusterFileName);
  ClusterFileName += ".cluster";
  DeltaFileName   += ".delta";

  //-- Open all the files
  std::ifstream RefFile(RefFileName);
  //  RefFile = File_Open (RefFileName.c_str(), "r");
  std::ifstream QryFile(QryFileName);
  //  QryFile = File_Open (QryFileName.c_str(), "r");
  std::ofstream ClusterFile, DeltaFile;
  if ( DO_DELTA ) {
    open_ofstream(DeltaFile, DeltaFileName);
    DeltaFile << RefFileName << ' ' << QryFileName << "\nNUCMER\n";
  } else {
    open_ofstream(ClusterFile, ClusterFileName);
    ClusterFile << RefFileName << QryFileName << "\nNUCMER\n";
  }
  auto print_delta = [&](const std::vector<Alignment>& Alignments,
                         const FastaRecord& Af, const FastaRecord& Bf) {
    printDeltaAlignments(Alignments, Af, Bf, DeltaFile);
  };
  auto print_clusters = [&](const synteny_list_type& Syntenys, const FastaRecord& Bf) {
    printSyntenys(Syntenys, Bf, ClusterFile);
  };

  //-- Generate the array of the reference sequences
  do {
    Af.push_back(FastaRecord());
  } while(Af.back().read_sequence(RefFile));
  Af.resize(Af.size() - 1);
  RefFile.close();

  if(Af.empty())
    parseAbort(RefFileName.c_str());

  //-- Process the input from <stdin> line by line
  int c = std::cin.peek();
  if(c != '>' && c != EOF) {
    std::cerr << "File must start with a '>'" << std::endl;
    exit(1);
  }
  for( ; c != EOF; c = std::cin.peek()) {
    // Read header
    std::cin.get();
    std::cin >> CurrIdB;
    if(CurrIdB.empty())
      parseAbort(Line + " - " + CurrIdB);
    std::getline(std::cin, Line);
    const char DirB = Line.find(" Reverse") == string::npos ? FORWARD_CHAR : REVERSE_CHAR; // the current query strand direction

    if(CurrIdB != Bf.Id() && !Syntenys.empty())
      merger.processSyntenys_each(Syntenys, Bf, print_clusters, print_delta);

    // Read in query sequence if needed. Must be in same order as for mummer
    while(CurrIdB != Bf.Id() && Bf.read_sequence(QryFile)) ;
    if(CurrIdB != Bf.Id())
      parseAbort("Query File did not find '" + Bf.Id() + "'. It is missing or not in correct order.");

    // Collect clusters in each synteny (same Id for ref and query)
    for(c = std::cin.peek(); c != '>' && c != EOF; c = std::cin.peek()) {
      IdA = nullptr;
      Cluster currCl(DirB);
      for( ; c != '#' && c != '>' && c != EOF; c = std::cin.peek()) {
        std::cin >> sA >> sB >> len; // Read match line
        if(!std::cin.good())
          parseAbort ("stdin" + to_string(cin.tellg()));
        ignore_line(std::cin); // Ignore rest of line

        //-- Re-map the reference coordinate back to its original sequence
        for ( Seqi = 0;   Seqi < Af.size() && sA > Af[Seqi].len(); ++Seqi)
          sA -= Af[Seqi].len() + 1; // extra +1 for the x at the end of each seq
        if ((size_t)Seqi >= Af.size()) {
          cerr << "ERROR: A MUM was found with a start coordinate greater than\n"
               << "       the sequence length, a serious error has occured.\n"
               << "       Please file a bug report\n";
          exit (EXIT_FAILURE);
        }
        //-- If the match spans across a sequence boundry
        if ( sA + len - 1 > Af[Seqi].len() || sA <= 0) {
          cerr << "WARNING: A MUM was found extending beyond the boundry of:\n"
               << "         Reference sequence '>" << Af[Seqi].Id() << "'\n\n"
               << "Please check that the '-n' option is activated on 'mummer2'\n"
               << "and try again, or file a bug report\n"
               << "Attempting to continue.\n";
          continue;
        }
        if(!IdA) {
          IdA = &Af[Seqi].Id();
          CurrSp = std::find_if(Syntenys.rbegin(), Syntenys.rend(), [=](const synteny_type& s) { return s.AfP->Id() == *IdA; });
          if(CurrSp == Syntenys.rend()) { // Not seen yet, create new synteny region
            Syntenys.push_back({ &Af[Seqi] });
            CurrSp = Syntenys.rbegin();
          } else if (CurrSp->AfP->len() != Af[Seqi].len() ) {
            cerr << "ERROR: The reference file may contain"
                 << " sequences with non-unique\n"
                 << "       header Ids, please check your input"
                 << " files and try again\n";
            exit (EXIT_FAILURE);
          }
        }
        if(*IdA != Af[Seqi].Id()) {
          cerr << "WARNING: A cluster was found straddling two reference sequences:\n"
               << "1) " << IdA << "\nand\n2) " << Af[Seqi].Id() << '\n'
               << "File a bug report\n";
          exit(EXIT_FAILURE);
        }
        if(len > 1)
          currCl.matches.push_back({ sA, sB, len });
      }
      CurrSp->clusters.push_back(std::move(currCl));
      if(c == '#')
        ignore_line(std::cin);
    }
  }
  if(!Syntenys.empty())
    merger.processSyntenys_each(Syntenys, Bf, print_clusters, print_delta);

  QryFile.close();

  return EXIT_SUCCESS;
}
