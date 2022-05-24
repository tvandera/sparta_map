// <ConfigParserYAML> -*- C++ -*-


#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <stack>
#include <vector>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/yaml.h>

#include "sparta/parsers/ConfigParser.hpp"
#include "sparta/simulation/Parameter.hpp"
#include "sparta/simulation/ParameterTree.hpp"
#include "sparta/simulation/TreeNodePrivateAttorney.hpp"

namespace YP = YAML; // Prevent collision with YAML class in ConfigParser namespace.


namespace sparta
{
    namespace ConfigParser
    {

        /*!
         * \brief Consumes a YAML file and assigns values to Parameters found
         * by searching a TreeNode-based device tree.
         * \note Opens a filestream immediately. Closes only when this class is
         * destructed.
         * \todo Use a logger
         *
         * Example:
         * \code{.cpp}
         * // Given some TreeNode* top;
         * YAML parser("input.yaml");
         * parser.consumeParameters(top);
         * \endcode
         */
        class YAML : public ConfigParser
        {

            typedef std::vector<TreeNode*> NodeVector; //! Vector representing the possible nodes during traversal

            /*!
             * \brief Predicate function for filtering nodes before assignment
             */
            typedef std::function<bool(const TreeNode*)> ApplyFilterPredicate;

        public:

            /*!
             * \brief YAML parser event handler. Implements yaml-cpp's
             * EventHandler interface to receive node events from the yaml
             * parser as it parses the input file.
             *
             * Internally, a stack is maintained based on traversal of a sparta
             * TreeNode-based device tree which is directed by the key node
             * names encountered while parsing the YAML file. These names are
             * interpreted as dot-separated path patterns pointing to 1 or more
             * nodes in the device tree below or above the current position.
             * When a YAML key is encountered that has a scalar or sequence of
             * scalars as its value, it is treated as a parameter and it is
             * assigned the value of that scalar/sequence.
             *
             * An alternative approach would be to examine the completed
             * tree generated by yaml-cpp and resolve its nodes to the given
             * sparta device tree, but building the intermediate tree is an
             * unnecessary step.
             */
            class EventHandler : public YP::EventHandler
            {

                /*!
                 * \brief Handles stack of parameters relavant to a sequence being parsed.
                 *
                 * Should support nested sequences such as: "[[1,2,3],[4,5],[]]"
                 *
                 * Generates a string of the yaml representing the sequence of
                 * tokens seen so that it can be reinterpreted as YAML.
                 */
                class SequenceStack {
                    std::string sequence_string_; //!< String rendering of the sequence so far.

                    //! Stack of entries pair<scalar elements, sequence parameter vector>
                    std::stack<std::pair<uint32_t, std::vector<ParameterBase*>>> params_;

                public:

                    /*!
                     * \brief Pushes another level of parameters to the stack and
                     */
                    void push(const std::vector<ParameterBase*>& v) {
                        if(params_.empty()){
                            sequence_string_ = "";
                        }
                        if(params_.size() && params_.top().first > 0){
                            sequence_string_ += ",";
                        }
                        sequence_string_ += "[";
                        params_.push({0, v});
                    }

                    void pop() {
                        params_.pop();
                        sequence_string_ += "]";
                        if(params_.size() > 0){
                            params_.top().first++;
                        }
                    }

                    void addValue(const std::string& val) {
                        sparta_assert(size() > 0);
                        if(params_.size()){
                            if(params_.top().first > 0){
                                sequence_string_ += ",";
                            }
                            params_.top().first++;
                        }
                        // Ensure all-whitespace or empty strings are quoted
                        if(val.find_first_not_of(" \t") == std::string::npos){
                            sequence_string_ += "\"";
                            sequence_string_ += val;
                            sequence_string_ += "\"";
                        }else{
                            sequence_string_ += val;
                        }
                    }

                    const std::string& getValue() const {
                        return sequence_string_;
                    }

                    bool empty() const {
                        return params_.empty();
                    }

                    size_t size() const {
                        return params_.size();
                    }

                    /*!
                     * \brief Gets the top of the stack alkong with a bool
                     */
                    const std::vector<ParameterBase*>& top() const {
                        return params_.top().second;
                    }

                    /*!
                     * \brief Gets the top of the stack alkong with a bool
                     */
                    std::vector<ParameterBase*>& top() {
                        return params_.top().second;
                    }
                };

                /*!
                 * \brief Highest allowed width of subtree_ (or any level of
                 * tree_stack_) in order to prevent excessive memory use or
                 * unaccceptable performance.
                 *
                 * It is assumed that any pattern-matched traveral which match
                 * more than this many nodes at a particular level is misusing
                 * the framework, or has uncovered a bug.
                 */
                static const size_t MAX_MATCHES_PER_LEVEL = 2000;

                // Config-file context information
                const std::string filename_;       //!< Name of input file
                NodeVector const trees_;           //!< Device tree associated with the document head

                // Tree-State
                NodeVector subtree_;               //!< Current possible nodes in the tree (children from 1 or more generations of pattern matching)
                uint32_t nesting_;                 //!< For debugging -  tree depth from top
                YP::NodeType::value cur_;          //!< Current node type associated with subtree_
                //std::stack<TreeNode*> tree_stack_; //!< Stack of treenodes
                std::stack<NodeVector> tree_stack_; //!< Stack of NodeVectors (copies of subtree_ at various depths) representing possible paths through tree while traversing with patterns
                std::vector<uint32_t> sequence_pos_; //!< Position in nested sequences. First element is position within a sequence
                //std::stack<std::vector<ParameterBase*>> seq_params_; //! Stack of parameters associated with of each level of nested sequence
                SequenceStack seq_params_; //! Stack of parameters associated with of each level of nested sequence
                std::stack<std::vector<std::string>> seq_vec_; //! Stack of vectors of values in nested sequences
                std::string last_val_;             //!< Prior scalar value
                bool verbose_;                     //!< Verbose parse mode
                std::vector<std::string> errors_;  //!< Errors encountered while running

                ParameterTree& ptree_;             //!< Extracted parameters in tree form
                std::vector<std::string>  include_paths_; //! Include paths to search for include statements
                std::stack<ParameterTree::Node*> pt_stack_; //!< Stack of contexts
                ParameterTree::Node* pt_node_;     //!< Current node in the ParameterTree
                bool allow_missing_nodes_;         //!< Allow missing nodes
                bool write_to_default_;            //!< Write to default values instead of current values
                ApplyFilterPredicate filter_predicate_; //!< Callback for applying the filter?

            public:

                /*!
                 * \brief Constructor
                 * \param filename Name of file to read from. This file will be
                 * opened immediately. If it cannot be opened, an exception is
                 * thrown
                 * \param device_trees vector of TreeNode* to use as the roots
                 * for parsing the input file. All top-level items in the input
                 * file will be resolved as descendents of this <device_tree>
                 * node.
                 * \param ptree ParameterTree to populate
                 * \param verbose Show verbose output
                 * \throws exception if filename cannot be opened for read
                 */
                EventHandler(const std::string& filename,
                             NodeVector device_trees,
                             ParameterTree& ptree,
                             const std::vector<std::string> & include_paths,
                             bool verbose) :
                    filename_(filename),
                    trees_(device_trees),
                    nesting_(0), // Must start at 0 because of assumptions when detecting singular scalars
                    cur_(YP::NodeType::Null),
                    verbose_(verbose),
                    ptree_(ptree),
                    include_paths_(include_paths),
                    pt_node_(ptree_.getRoot()),
                    allow_missing_nodes_(false),
                    write_to_default_(false),
                    // Allow all nodes unless otherwise specified. This allows this function to be called without checking its validityu
                    filter_predicate_([](const TreeNode*){return true;})
                {
                    sparta_assert(device_trees.size() != 0);


                }

                /*!
                 * \brief Configure this event handler to allow or disallow missing TreeNodes while
                 * parsing
                 */
                void allowMissingNodes(bool allow) { allow_missing_nodes_ = allow; }

                /*!
                 * \brief Configure this event handler to write to default values instead of the
                 * current value of each parameter affected
                 */
                void writeToDefault(bool def) { write_to_default_ = def; }

                /*!
                 * \brief Set a predicate function which is checked before finally writing a value
                 * to any parameter in the tree. This lets the parameter consumption operate on only
                 * a subset of the parameter tree while "masking" or filtering some parts off.
                 * \note This does not effect the virtual parameter tree.
                 */
                void setParameterApplyFilter(ApplyFilterPredicate prd) { filter_predicate_ = prd; }

                /*!
                 * \brief Does this event handler allow missing TreeNodes while parsing. Defaults to
                 * false
                 */
                bool doesAllowMissingNodes() const { return allow_missing_nodes_; }

                //! Returns a reference to the internal vector of errors encountered during events.
                const std::vector<std::string>& getErrors()
                {
                    return errors_;
                }

                const ParameterTree& getParameterTree() const {
                    return ptree_;
                }

                //! Dummy method that returns self in order to make log statements within code more readable.
                EventHandler& verbose()
                {
                    return *this;
                }

                //! Logging operator to cout.
                //! \tparam T type to insert in output stream
                template <class T>
                EventHandler& operator<<(const T& r) {
                    if(verbose_){
                        std::cout << r;
                    }
                    return *this;
                }

                //! Help with some ostream modifiers. Template could not deduce parameters correctly.
                EventHandler& operator<<(std::ostream&(*r)(std::ostream&)) {
                    if(verbose_){
                        std::cout << r;
                    }
                    return *this;
                }

                //! Help with some ios modifiers. Template could not deduce parameters correctly.
                EventHandler& operator<<(std::ios&(*r)(std::ios&)) {
                    if(verbose_){
                        std::cout << r;
                    }
                    return *this;
                }

                //! Help with some ios modifiers. Template could not deduce parameters correctly.
                EventHandler& operator<<(std::ios_base&(*r)(std::ios_base&)) {
                    if(verbose_){
                        std::cout << r;
                    }
                    return *this;
                }

                //! Handle document start YAML node from parser
                void OnDocumentStart(const YP::Mark& mark) override final
                {
                    (void) mark;

                    // Start subtree at starting tree list and pt_node_ at ptree_.getRoot()
                    subtree_.insert(subtree_.end(), trees_.begin(), trees_.end());
                    pt_node_ = ptree_.getRoot();
                    cur_ = YP::NodeType::Null;
                    while(!tree_stack_.empty()){tree_stack_.pop();}
                    while(!pt_stack_.empty()){pt_stack_.pop();}
                    while(!seq_vec_.empty()){seq_vec_.pop();}
                    while(!seq_params_.empty()){seq_params_.pop();}
                    last_val_ = "";

                    verbose() << indent_() << "(" << subtree_.size() << ") vptn:" << (pt_node_ ? pt_node_->getPath() : "<null>") << " + DocumentStart @" << mark.line << std::endl;

                    // DocumentStart must increment nesting because code for detecting standalone
                    // scalars requires nesting_ = 1
                    nesting_++;
                }

                //! Handle document end YAML node from parser
                void OnDocumentEnd() override final
                {
                    if(subtree_.size() > 0){
                        verbose() << indent_() << "(" << subtree_.size() << ") vptn:" << (pt_node_ ? pt_node_->getPath() : "<null>") << " + DocumentEnd" << std::endl;
                    }else{
                        verbose() << indent_() << "(commented)" << " vptn:" << (pt_node_ ? pt_node_->getPath() : "<null>") << " + DocumentEnd" << std::endl;
                    }
                    nesting_--;

                    // Everything is re-cleared within OnDocumentStart
                    // If tree_stack_ is not empty, something was incorrect
                    sparta_assert(tree_stack_.empty());
                    sparta_assert(pt_stack_.empty());
                }

                //! Handle NULL YAML node from parser
                void OnNull(const YP::Mark& mark, YP::anchor_t anchor) override final
                {
                    (void) anchor;

                    if(subtree_.size() > 0){
                        verbose() << indent_() << "(" << subtree_.size() << ") vptn:"
                                  << (pt_node_ ? pt_node_->getPath() : "<null>") << " + NULL @" << mark.line << std::endl;
                    }else{
                        verbose() << indent_() << "(commented)" << " vptn:"
                                  << (pt_node_ ? pt_node_->getPath() : "<null>") << " + NULL @" << mark.line << std::endl;
                    }

                    // TODO: Determine if this is proper handling of NULLs

                    std::stringstream ss;
                    // Note: the last scalar cannot be shown here. It is NOT last_val_
                    ss << "Encountered a YAML Null token, which should not be allowed. This may be "
                        "an incorrect attempt to specify a sequence using { }. The syntax for "
                        "specifying a sequence is [a,b,c,d,e].";
                    ss << markToString_(mark);
                    errors_.push_back(ss.str());

                    last_val_ = "";
                }

                //! Handle Aliase YAML node from parser
                void OnAlias(const YP::Mark& mark, YP::anchor_t anchor) override final
                {
                    (void) anchor;
                    if(subtree_.size() > 0){
                        verbose() << indent_() << "(" << subtree_.size() << ") vptn:"
                                  << (pt_node_ ? pt_node_->getPath() : "<null>") << " + Alias @" << mark.line << std::endl;
                    }else{
                        verbose() << indent_() << "(commented)" << " vptn:"
                                  << (pt_node_ ? pt_node_->getPath() : "<null>") << " + Alias @" << mark.line << std::endl;
                    }
                    throw ParameterException("YAML Aliases are not yet supported in SPARTA");
                }

                //! Handle Scalar (key or value) YAML node from parser
                void OnScalar(const YP::Mark& mark, const std::string& tag,
                              YP::anchor_t anchor, const std::string& value)  override final;

                //! Handle SequenceStart YAML node from parser
                void OnSequenceStart(const YP::Mark& mark, const std::string& tag,
                                     YP::anchor_t anchor, YP::EmitterStyle::value style) override final;

                //! Handle SequenceEnd YAML node from parser
                void OnSequenceEnd() override final;

                //! Handle MapStart YAML node from parser
                void OnMapStart(const YP::Mark& mark, const std::string& tag,
                                YP::anchor_t anchor, YP::EmitterStyle::value style) override final;

                //! Handle MapEnd YAML node from parser
                void OnMapEnd() override final;


            private:

                void findNextGeneration_(NodeVector& current, const std::string& pattern,
                                         NodeVector& next, const YP::Mark& mark);
                /*!
                 * \brief Return a stirng containing spaces as a multiple of the
                 * nesting_ level.
                 */
                std::string indent_()
                {
                    std::stringstream ss;
                    for(uint32_t i=0;i<nesting_;++i){
                        ss << "  ";
                    }
                    return ss.str();
                }

                /*!
                 * \brief Sets the given sequence YAML node <node> as the value
                 * of the parameter described by <param_path> relative to the
                 * current node <subtree>.
                 * \param subtree Current node.
                 * \param param_path Path (pattern) relative to <subtree> to a
                 * node which is a sparta::Parameter.
                 * \param node The value to assign to the parameter decribed by
                 * <subtree> and <param_path>.
                 */
                void applyArrayParameter(TreeNode* subtree,
                                         const std::string& param_path,
                                         const YP::Node& node);
                /*!
                 * \brief Consumes a file based on an include directives destination.
                 * \param pfilename YAML file to read
                 * \param device_trees Vector of TreeNodes to act as roots of
                 * the filename being read. This allows includes to be scoped to specific
                 * nodes. The file will be parsed once an applied to all roots in
                 * device_trees.
                 */
                void handleIncludeDirective(const std::string& filename, NodeVector& device_trees, ParameterTree::Node* ptn);

                //! Adds the mark info from current node to a SpartaException.
                //! Includes filename, line, and column
                void addMarkInfo_(SpartaException& ex, const YP::Mark& mark){
                    ex << " in file " << filename_ << ":" << (mark.line + 1)
                       << " col:" << mark.column;
                }

                //! Adds the mark info from current node to a SpartaException.
                //! Includes filename, line, and column
                std::string markToString_(const YP::Mark& mark, bool verbose=true){
                    std::stringstream ss;
                    if(verbose){
                        ss << " in file ";
                    }
                    ss << filename_ << ":" << mark.line
                       << " col:" << mark.column;
                    return ss.str();
                }

            }; // class EventHandler

            /*!
             * \brief Constructor for a YAML parameter file parser
             * \param filename Path of file to consume parameters from. Must be readable
             */
            YAML(const std::string& filename,
                 const std::vector<std::string> & include_paths) :
                ConfigParser(filename),
                fin_(filename.c_str(), std::ios_base::in),
                parser_(),
                filename_(filename),
                include_search_dirs_(include_paths),
                allow_missing_nodes_(false),
                filter_predicate_([](const TreeNode*){return true;})
            {
                if(!fin_.is_open()){
                    if(include_search_dirs_.empty()){
                        include_search_dirs_.emplace_back(".");
                    }
                    for(const auto & search_dir : include_search_dirs_){
                        const std::string full_filename = search_dir + "/" + filename_;
                        fin_.open(full_filename, std::ios_base::in);
                        if (fin_.is_open()) {
                            break;
                        }
                    }
                }
                if(false == fin_.is_open()){
                    throw ParameterException("Failed to open YAML Configuration file for read \"")
                        << filename << "\"";
                }
                parser_.reset(new YP::Parser(fin_));
            }

            YAML() = delete;

            YAML(const YAML&) = delete;

            YAML& operator=(const YAML&) = delete;

            /*!
             * \brief Destructor
             * \post Output stream will be closed
             */
            ~YAML()
            {
            }

            /*!
             * \brief Configure this parser to allow or disallow missing TreeNodes while parsing
             */
            void allowMissingNodes(bool allow) { allow_missing_nodes_ = allow; }

            /*!
             * \brief Does this parser allow missing TreeNodes while parsing. Defaults to false
             */
            bool doesAllowMissingNodes() const { return allow_missing_nodes_; }


            /*!
             * \brief Set a predicate function which is checked before finally writing a value
             * to any parameter in the tree. This lets the parameter consumption operate on only
             * a subset of the parameter tree while "masking" or filtering some parts off.
             * \note This does not effect the virtual parameter tree.
             */
            void setParameterApplyFilter(ApplyFilterPredicate prd) { filter_predicate_ = prd; }

            /*!
             * \brief Reads parameters from YAML file.
             * \param device_tree Any node in a device tree to use as the root
             * for resolving node names found in YAML file to device tree nodes.
             * node names at the outer-most level in the YAML file will be
             * resolved to descendants of this <device_tree> node. Must not be 0.
             * \param verbose Display verbose output messages to stdout/stderr
             * \throw SpartaException on failure
             * \note Filestream is NOT closed after this call.
             *
             * Any key nodes in the input file which cannot be resolved to at least
             * 1 device tree node will generate an exception.
             *
             * Any leaf value or sequence in the input file will be treated as a
             * parameter value. If they key with which that value is associated
             * in the input file does not resolve to a Parameter node in the
             * device tree, it will generate an exception.
             */
            void consumeParameters(TreeNode* device_tree, bool verbose=false)
            {
                sparta_assert(device_tree); // device_tree must not be 0

                NodeVector v{device_tree};
                consumeParameters(v, verbose);
            }

            /*!
             * \brief Reads parameters from a YAML fle.
             * \see void consumeParameters(TreeNode*, bool)
             *
             * Operates on a vector of tree roots instead of a single tree
             */
            void consumeParameters(NodeVector& device_trees, bool verbose=false)
            {
                sparta_assert(device_trees.size() > 0);

                if(verbose){
                    std::cout << "Reading parameters from \"" << filename_ << "\"" << std::endl;
                }

                EventHandler handler(filename_, device_trees, ptree_, include_search_dirs_, verbose);
                handler.allowMissingNodes(allow_missing_nodes_);
                handler.setParameterApplyFilter(filter_predicate_);
                while(parser_->HandleNextDocument(*((YP::EventHandler*)&handler))) {}

                if(handler.getErrors().size() != 0){
                    SpartaException ex("One or more errors detected while consuming the parameter file:\n");
                    for(const std::string& es : handler.getErrors()){
                        ex << es << '\n';
                    }
                    throw  ex;
                }

                if(verbose){
                    std::cout << "Done reading parameters from \"" << filename_ << "\"" << std::endl;
                }
            }

            const ParameterTree& getParameterTree() const {
                return ptree_;
            }

        private:

            std::ifstream fin_;          //!< Input file stream. Opened at construction
            std::unique_ptr<YP::Parser> parser_; //!< YP::Parser to which events will be written
            const std::string filename_; //!< For recalling errors
            ParameterTree ptree_;        //!< Extracted parameters in tree form
            std::vector<std::string> include_search_dirs_; //!< The include paths for include directives found in yaml files
            bool allow_missing_nodes_; //!< Allow missing TreeNodes when parsing
            ApplyFilterPredicate filter_predicate_; //!< Callback for applying the filter?

        }; // class YAML

    }; // namespace ConfigParser

}; // namespace sparta

// __CONFIG_PARSER_YAML)_

/*!
  \page YAML_Parameter_Files

  YAML Parameters can be specified before
*/