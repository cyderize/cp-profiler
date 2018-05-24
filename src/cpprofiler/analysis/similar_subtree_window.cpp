#include "similar_subtree_window.hh"

#include <QGraphicsView>
#include <QVBoxLayout>
#include <QSplitter>
#include <QGraphicsSimpleTextItem>
#include <QDebug>
#include <QLabel>
#include <QComboBox>
#include <QAction>

#include <iostream>
#include <algorithm>
#include <set>

#include "../tree/shape.hh"
#include "../tree/structure.hh"
#include "../tree/layout.hh"
#include "../tree/node_tree.hh"
#include "../utils/tree_utils.hh"

#include "subtree_pattern.hh"
#include "../tree/subtree_view.hh"

#include "histogram_scene.hh"
#include "../utils/perf_helper.hh"

using namespace cpprofiler::tree;

using std::vector;

namespace cpprofiler
{
namespace analysis
{

SimilarSubtreeWindow::SimilarSubtreeWindow(QWidget *parent, const tree::NodeTree &nt, const tree::Layout &lo)
    : QDialog(parent), m_nt(nt), m_lo(lo)
{

    m_histogram.reset(new HistogramScene);
    auto hist_view = new QGraphicsView{this};

    hist_view->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    m_subtree_view.reset(new SubtreeView{nt});

    hist_view->setScene(m_histogram->scene());

    auto globalLayout = new QVBoxLayout{this};

    {
        auto settingsLayout = new QHBoxLayout{};
        globalLayout->addLayout(settingsLayout);
        settingsLayout->addWidget(new QLabel{"Similarity Criteria:"}, 0, Qt::AlignRight);

        auto typeChoice = new QComboBox();
        typeChoice->addItems({"contour", "subtree"});
        settingsLayout->addWidget(typeChoice);

        connect(typeChoice, &QComboBox::currentTextChanged, [this](const QString &str) {
            if (str == "contour")
            {
                m_sim_type = SimilarityType::SHAPE;
            }
            else if (str == "subtree")
            {
                m_sim_type = SimilarityType::SUBTREE;
            }
            analyse();
        });
    }

    auto splitter = new QSplitter{this};

    splitter->addWidget(hist_view);
    splitter->addWidget(m_subtree_view->widget());
    splitter->setSizes(QList<int>{1, 1});

    globalLayout->addWidget(splitter, 1);

    globalLayout->addWidget(&path_line1_);
    globalLayout->addWidget(&path_line2_);

    analyse();

    /// see if I can add action here

    auto keyDown = new QAction{"Press down", this};
    keyDown->setShortcuts({QKeySequence("Down"), QKeySequence("J")});
    addAction(keyDown);

    connect(keyDown, &QAction::triggered, m_histogram.get(), &HistogramScene::nextPattern);

    auto keyUp = new QAction{"Press up", this};
    keyUp->setShortcuts({QKeySequence("Up"), QKeySequence("K")});
    addAction(keyUp);

    connect(keyUp, &QAction::triggered, m_histogram.get(), &HistogramScene::prevPattern);

    connect(keyUp, &QAction::triggered, [this]() {
        update();
    });
}

SimilarSubtreeWindow::~SimilarSubtreeWindow() = default;

struct ShapeInfo
{
    NodeID nid;
    const Shape &shape;
};

struct CompareShapes
{
  public:
    bool operator()(const ShapeInfo &si1, const ShapeInfo &si2) const
    {

        const auto &s1 = si1.shape;
        const auto &s2 = si2.shape;

        if (s1.height() < s2.height())
            return true;
        if (s1.height() > s2.height())
            return false;

        for (int i = 0; i < s1.height(); ++i)
        {
            if (s1[i].l < s2[i].l)
                return false;
            if (s1[i].l > s2[i].l)
                return true;
            if (s1[i].r < s2[i].r)
                return true;
            if (s1[i].r > s2[i].r)
                return false;
        }
        return false;
    }
};

static std::vector<int> calculateSubtreeSizes(const NodeTree &nt)
{

    const int nc = nt.nodeCount();

    std::vector<int> sizes(nc);

    std::function<void(NodeID)> countDescendants;

    /// Count descendants plus one (the node itself)
    countDescendants = [&](NodeID n) {
        auto nkids = nt.childrenCount(n);
        if (nkids == 0)
        {
            sizes[n] = 1;
        }
        else
        {
            int count = 1; // the node itself
            for (auto alt = 0u; alt < nkids; ++alt)
            {
                const auto kid = nt.getChild(n, alt);
                countDescendants(kid);
                count += sizes[kid];
            }
            sizes[n] = count;
        }
    };

    const auto root = nt.getRoot();
    countDescendants(root);

    return sizes;
}

static std::vector<SubtreePattern> runSimilarShapes(const NodeTree &tree, const Layout &lo)
{

    perfHelper.begin("similar shapes body");

    std::multiset<ShapeInfo, CompareShapes> shape_set;

    auto node_order = utils::any_order(tree);

    for (const auto nid : node_order)
    {
        shape_set.insert({nid, lo.getShape(nid)});
    }

    perfHelper.end();
    perfHelper.begin("similar shapes result");

    auto sizes = calculateSubtreeSizes(tree);

    std::vector<SubtreePattern> shapes;

    auto it = shape_set.begin();
    auto end = shape_set.end();

    while (it != end)
    {
        auto upper = shape_set.upper_bound(*it);

        const int height = it->shape.height();

        auto pattern = SubtreePattern{{}, height, 0};
        for (; it != upper; ++it)
        {
            pattern.m_nodes.emplace_back(it->nid);
        }

        /// TODO: use a caching for calculating this
        pattern.size_ = sizes.at(pattern.first());

        shapes.push_back(std::move(pattern));
    }

    perfHelper.end();
    // perfHelper.end();

    return shapes;
}

using Group = std::vector<NodeID>;
using std::vector;

struct Partition
{

    vector<Group> processed;
    vector<Group> remaining;

    Partition(vector<Group> &&proc, vector<Group> &&rem)
        : processed(proc), remaining(rem)
    {
    }
};

std::ostream &operator<<(std::ostream &out, const Group &group)
{

    const auto size = group.size();
    if (size == 0)
    {
        return out;
    }
    for (auto i = 0; i < size - 1; ++i)
    {
        out << (int)group[i] << " ";
    }
    if (size > 0)
    {
        out << (int)group[size - 1];
    }
    return out;
}

std::ostream &operator<<(std::ostream &out, const vector<Group> &groups)
{

    const auto size = groups.size();

    if (size == 0)
        return out;
    for (auto i = 0; i < size - 1; ++i)
    {
        out << groups[i];
        out << "|";
    }

    out << groups[size - 1];

    return out;
}

std::ostream &operator<<(std::ostream &out, const Partition &partition)
{

    out << "[";
    out << partition.processed;
    out << "] [";
    out << partition.remaining;
    out << "]";

    return out;
}

enum class LabelOption
{
    IGNORE_LABEL,
    VARS,
    FULL
};

static Partition initialPartition(const NodeTree &nt)
{
    /// separate leaf nodes from internal
    /// NOTE: this assumes that branch nodes have at least one child!
    Group failed_nodes;
    Group solution_nodes;
    Group branch_nodes;
    {
        auto nodes = utils::any_order(nt);

        for (auto nid : nodes)
        {
            auto status = nt.getStatus(nid);
            switch (status)
            {
            case NodeStatus::FAILED:
            {
                failed_nodes.push_back(nid);
            }
            break;
            case NodeStatus::SOLVED:
            {
                solution_nodes.push_back(nid);
            }
            break;
            case NodeStatus::BRANCH:
            {
                branch_nodes.push_back(nid);
            }
            break;
            default:
            {
                /// ignore other nodes for now
            }
            break;
            }
        }
    }

    Partition result{{}, {}};

    if (failed_nodes.size() > 0)
    {
        result.processed.push_back(std::move(failed_nodes));
    }

    if (solution_nodes.size() > 0)
    {
        result.processed.push_back(std::move(solution_nodes));
    }

    if (branch_nodes.size() > 0)
    {
        result.remaining.push_back(std::move(branch_nodes));
    }

    return result;
}

static bool contains(const vector<NodeID> &vec, NodeID nid)
{
    for (auto el : vec)
    {
        if (el == nid)
            return true;
    }
    return false;
}

static vector<Group> separate_marked(const vector<Group> &rem_groups, const vector<NodeID> &marked)
{

    vector<Group> res_groups;

    for (auto &group : rem_groups)
    {

        Group g1; /// marked
        Group g2; /// not marked

        for (auto nid : group)
        {
            if (contains(marked, nid))
            {

                // qDebug() << nid << "is marked";
                g1.push_back(nid);
            }
            else
            {

                // qDebug() << nid << "is NOT marked";
                g2.push_back(nid);
            }
        }

        if (g1.size() > 0)
        {
            res_groups.push_back(std::move(g1));
        }
        if (g2.size() > 0)
        {
            res_groups.push_back(std::move(g2));
        }
    }

    return res_groups;
}

/// go through processed groups of height 'h' and
/// mark their parent nodes as candidates for separating
static void partition_step(const NodeTree &nt, Partition &p, int h, std::vector<int> &height_info)
{

    for (auto &group : p.processed)
    {

        /// Check if the group should be skipped
        /// Note that processed nodes are assumed to be of the same height
        if (group.size() == 0 || height_info.at(group[0]) != h)
        {
            continue;
        }

        const auto max_kids = std::accumulate(group.begin(), group.end(), 0, [&nt](int cur, NodeID nid) {
            auto pid = nt.getParent(nid);
            return std::max(cur, nt.childrenCount(pid));
        });

        int alt = 0;
        for (auto alt = 0; alt < max_kids; ++alt)
        {

            vector<NodeID> marked;

            for (auto nid : group)
            {
                if (nt.getAlternative(nid) == alt)
                {
                    /// Mark the parent of nid to separate
                    auto pid = nt.getParent(nid);
                    marked.push_back(pid);
                }
            }

            /// separate marked nodes from their groups
            p.remaining = separate_marked(p.remaining, marked);
        }
    }
}

static void set_as_processed(const NodeTree &nt, Partition &partition, int h, std::vector<int> &height_info)
{

    /// Note that in general a group can contain subtrees of
    /// different height; however, since the subtrees of height 'h'
    /// have already been processed, any group that contains such a
    /// subtree is processed and contains only subtrees of height 'h'.
    auto should_stay = [&nt, &height_info, h](const Group &g) {
        auto nid = g.at(0);

        return height_info.at(nid) != h;
    };

    auto &rem = partition.remaining;

    auto first_to_move = std::partition(rem.begin(), rem.end(), should_stay);

    move(first_to_move, rem.end(), std::back_inserter(partition.processed));
    rem.resize(distance(rem.begin(), first_to_move));
}

static int calculateHeightOf(NodeID nid, const NodeTree &nt, std::vector<int> &height_info)
{

    int cur_max = 0;

    auto kids = nt.childrenCount(nid);

    if (kids == 0)
    {
        cur_max = 1;
    }
    else
    {
        for (auto alt = 0; alt < nt.childrenCount(nid); ++alt)
        {
            auto child = nt.getChild(nid, alt);
            cur_max = std::max(cur_max, calculateHeightOf(child, nt, height_info) + 1);
        }
    }

    height_info[nid] = cur_max;

    return cur_max;
}

/// TODO: make sure the structure isn't changing anymore
/// TODO: this does not work correctly for n-ary trees yet
static std::vector<SubtreePattern> runIdenticalSubtrees(const NodeTree &nt)
{

    auto label_opt = LabelOption::IGNORE_LABEL;

    std::vector<int> height_info(nt.nodeCount());

    auto max_height = calculateHeightOf(nt.getRoot(), nt, height_info);

    auto max_depth = nt.depth();

    auto cur_height = 1;

    // 0) Initial Partition
    auto partition = initialPartition(nt);

    while (cur_height != max_height)
    {

        partition_step(nt, partition, cur_height, height_info);

        /// By this point, all subtrees of height 'cur_height + 1'
        /// should have been processed
        set_as_processed(nt, partition, cur_height + 1, height_info);

        ++cur_height;
    }

    /// Construct the result in the appropriate form
    std::vector<SubtreePattern> result;
    result.reserve(partition.processed.size());

    for (auto &&group : partition.processed)
    {
        auto height = height_info.at(group[0]);
        result.push_back({std::move(group), height});
    }

    return result;
}

static vector<SubtreePattern> eliminateSubsumed(const NodeTree &tree, const vector<SubtreePattern> &patterns)
{

    std::set<NodeID> marked;

    for (const auto &pattern : patterns)
    {
        for (auto nid : pattern.nodes())
        {

            const auto kids = tree.childrenCount(nid);
            for (auto alt = 0; alt < kids; ++alt)
            {
                auto kid = tree.getChild(nid, alt);
                marked.insert(kid);
            }
        }
    }

    vector<SubtreePattern> result;

    for (const auto &pattern : patterns)
    {

        const auto &nodes = pattern.nodes();

        /// Determine if all nodes are in 'marked':
        auto subset = true;

        for (auto nid : nodes)
        {
            if (marked.find(nid) == marked.end())
            {
                subset = false;
                break;
            }
        }

        if (!subset)
        {
            result.push_back(pattern);
        }
    }

    return result;
}

void SimilarSubtreeWindow::analyse()
{

    // analyse_shapes();

    /// TODO: make sure building is finished

    auto patterns = std::vector<SubtreePattern>{};

    switch (m_sim_type)
    {
    case SimilarityType::SUBTREE:
    {
        patterns = runIdenticalSubtrees(m_nt);
    }
    break;
    case SimilarityType::SHAPE:
    {
        patterns = runSimilarShapes(m_nt, m_lo);
    }
    }

    /// make sure there are only patterns of cardinality > 1

    auto new_end = std::remove_if(patterns.begin(), patterns.end(), [](const SubtreePattern &pattern) {
        return pattern.count() <= 1;
    });

    patterns.resize(std::distance(patterns.begin(), new_end));

    patterns = eliminateSubsumed(m_nt, patterns);

    perfHelper.begin("set patterns");

    m_histogram->reset();
    m_histogram->setPatterns(std::move(patterns));
    m_histogram->drawPatterns();

    perfHelper.end();

    connect(m_histogram.get(), &HistogramScene::should_be_highlighted,
            this, &SimilarSubtreeWindow::should_be_highlighted);

    connect(m_histogram.get(), &HistogramScene::pattern_selected,
            m_subtree_view.get(), &SubtreeView::setNode);

    connect(m_histogram.get(), &HistogramScene::should_be_highlighted,
            this, &SimilarSubtreeWindow::updatePathDiff);

    // m_histogram->reset();
}

/// A wrapper around std::set_intersection; copying is intended
static vector<Label> set_intersect(vector<Label> v1, vector<Label> v2)
{
    /// set_intersection requires the resulting set to be
    /// at least as large as the smallest of the two sets
    vector<Label> res(std::min(v1.size(), v2.size()));

    std::sort(begin(v1), end(v1));
    std::sort(begin(v2), end(v2));

    auto it = std::set_intersection(begin(v1), end(v1), begin(v2), end(v2),
                                    begin(res));
    res.resize(it - begin(res));

    return res;
}

/// A wrapper around std::set_symemtric_diff; copying is intended
static vector<Label> set_symmetric_diff(vector<Label> v1, vector<Label> v2)
{
    vector<Label> res(v1.size() + v2.size());

    /// vectors must be sorted
    std::sort(begin(v1), end(v1));
    std::sort(begin(v2), end(v2));

    /// fill `res` with elements not present in both v1 and v2
    auto it = std::set_symmetric_difference(begin(v1), end(v1), begin(v2), end(v2),
                                            begin(res));

    res.resize(it - begin(res));

    return res;
}

/// Calculate unique labels for both paths (present in one but not in the other)
static std::pair<vector<Label>, vector<Label>>
getLabelDiff(const std::vector<Label> &path1,
             const std::vector<Label> &path2)
{
    auto diff = set_symmetric_diff(path1, path2);

    auto unique_1 = set_intersect(path1, diff);

    auto unique_2 = set_intersect(path2, diff);

    return std::make_pair(std::move(unique_1), std::move(unique_2));
}

/// Walk up the tree constructing label path
static std::vector<Label> labelPath(NodeID nid, const tree::NodeTree &tree)
{
    std::vector<Label> label_path;
    while (nid != NodeID::NoNode)
    {
        auto label = tree.getLabel(nid);
        if (label != emptyLabel)
        {
            label_path.push_back(label);
        }
        nid = tree.getParent(nid);
    }

    return label_path;
}

void SimilarSubtreeWindow::updatePathDiff(const std::vector<NodeID> &nodes)
{
    if (nodes.size() < 2)
        return;

    auto path_l = labelPath(nodes[0], m_nt);
    auto path_r = labelPath(nodes[1], m_nt);

    auto diff_pair = getLabelDiff(path_l, path_r);

    /// unique labels on path_l concatenated
    std::string text_l;

    for (auto &label : diff_pair.first)
    {
        text_l += label + " ";
    }

    std::string text_r;
    for (auto &label : diff_pair.second)
    {
        text_r += label + " ";
    }

    path_line1_.setText(text_l.c_str());
    path_line2_.setText(text_r.c_str());
}

} // namespace analysis
} // namespace cpprofiler