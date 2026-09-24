#pragma once
#include "Viewshed.h"

// How far an approximate viewshed is from an exact reference: the fast viewshed
// against the naive one, and any later approximation against its reference.
//
// Counting differing cells over every confident cell says little, because most
// cells are usually hidden: where 95% of the grid is hidden, a "viewshed" that
// answers NotVisible everywhere differs on under 5% of cells. So the rates below
// divide by the cells either viewshed finds visible -- a measure that doesn't
// shrink when little is visible -- and the counts behind them are kept, one
// direction at a time.
//
// Each differing cell is also placed relative to the reference's visibility edge:
// a cell is on the edge when a confident 8-neighbour gets the other answer from
// the reference. There, the approximation's answer is the one the reference gives
// right next door -- the boundary drawn one cell off. The reference moves its own
// boundary that far when the observer moves a few metres. Off the edge, no
// neighbour agrees with the approximation: it is wrong by more than a cell.
struct ViewshedAgreement
{
    // False when the two grids differ in size; every count is then zero.
    bool sameGrid = false;

    // Cells both viewsheds answer with confidence; only these are compared.
    long long comparedCells = 0;

    long long referenceVisible = 0;
    long long approximateVisible = 0;

    // The two directions of disagreement, kept apart.
    long long approximateOnlyVisible = 0;   // approximate says Visible, reference NotVisible
    long long referenceOnlyVisible = 0;     // reference says Visible, approximate NotVisible

    // Differing cells on the reference's visibility edge.
    long long differingOnEdge = 0;

    long long Differing() const { return approximateOnlyVisible + referenceOnlyVisible; }

    long long DifferingOffEdge() const { return Differing() - differingOnEdge; }

    // Cells at least one of the two finds visible: both, plus either one alone.
    long long VisibleInEither() const
    {
        long long both = (approximateVisible + referenceVisible - Differing()) / 2;
        return both + Differing();
    }

    // Every difference, over the cells either finds visible. 0 when neither finds any.
    double DisagreementRate() const
    {
        return VisibleInEither() == 0 ? 0.0 : (double)Differing() / VisibleInEither();
    }

    // Differences off the reference's edge, over the cells either finds visible. 0 when neither finds any.
    double OffEdgeRate() const
    {
        return VisibleInEither() == 0 ? 0.0 : (double)DifferingOffEdge() / VisibleInEither();
    }

    // The share of differences on the reference's edge. 1 when nothing differs.
    double OnEdgeFraction() const
    {
        return Differing() == 0 ? 1.0 : (double)differingOnEdge / Differing();
    }
};

// Compares an approximate viewshed with its exact reference, cell for cell.
// Cells either one has no confident answer for are left out, and so are
// neighbours without a confident reference answer.
//
// Complexity: O(rows * cols), up to eight reference cells read per differing cell.
// Thread-safety: reads its arguments only; safe to call concurrently.
inline ViewshedAgreement CompareViewsheds(const ViewshedResult& approximate, const ViewshedResult& reference)
{
    ViewshedAgreement agreement;
    const auto& a = approximate.visible;
    const auto& r = reference.visible;
    int rows = (int)r.size();
    int cols = rows == 0 ? 0 : (int)r[0].size();
    if ((int)a.size() != rows) return agreement;
    for (int row = 0; row < rows; row++)
    {
        if ((int)a[row].size() != cols || (int)r[row].size() != cols) return agreement;
    }
    agreement.sameGrid = true;

    // Whether a confident 8-neighbour of (row, col) gets the other answer from the reference.
    auto onReferenceEdge = [&](int row, int col)
    {
        bool sees = r[row][col] == CellVisibility::Visible;
        for (int dRow = -1; dRow <= 1; dRow++)
        {
            for (int dCol = -1; dCol <= 1; dCol++)
            {
                int nRow = row + dRow, nCol = col + dCol;
                if ((dRow == 0 && dCol == 0) || nRow < 0 || nCol < 0 || nRow >= rows || nCol >= cols) continue;
                CellVisibility neighbour = r[nRow][nCol];
                if (IsConfident(neighbour) && (neighbour == CellVisibility::Visible) != sees) return true;
            }
        }
        return false;
    };

    for (int row = 0; row < rows; row++)
    {
        for (int col = 0; col < cols; col++)
        {
            if (!IsConfident(a[row][col]) || !IsConfident(r[row][col])) continue;
            agreement.comparedCells++;

            bool approximateSees = a[row][col] == CellVisibility::Visible;
            bool referenceSees = r[row][col] == CellVisibility::Visible;
            agreement.approximateVisible += approximateSees;
            agreement.referenceVisible += referenceSees;
            if (approximateSees == referenceSees) continue;

            if (approximateSees) agreement.approximateOnlyVisible++;
            else agreement.referenceOnlyVisible++;
            if (onReferenceEdge(row, col)) agreement.differingOnEdge++;
        }
    }
    return agreement;
}
