/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2008 Andreas Gaida
 Copyright (C) 2008 Ralph Schreyer
 Copyright (C) 2008, 2009, 2010 Klaus Spanderen

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include "fdmlinearop.hpp"
#include "utilities.hpp"

#include <ql/quotes/simplequote.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/processes/hestonprocess.hpp>
#include <ql/processes/hullwhiteprocess.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/processes/hybridhestonhullwhiteprocess.hpp>
#include <ql/math/interpolations/bilinearinterpolation.hpp>
#include <ql/math/interpolations/bicubicsplineinterpolation.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/models/equity/hestonmodel.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/mchestonhullwhiteengine.hpp>
#include <ql/methods/finitedifferences/finitedifferencemodel.hpp>
#include <ql/math/matrixutilities/bicgstab.hpp>
#include <ql/methods/finitedifferences/schemes/douglasscheme.hpp>
#include <ql/methods/finitedifferences/schemes/hundsdorferscheme.hpp>
#include <ql/methods/finitedifferences/schemes/impliciteulerscheme.hpp>
#include <ql/methods/finitedifferences/schemes/craigsneydscheme.hpp>
#include <ql/methods/finitedifferences/meshers/uniformgridmesher.hpp>
#include <ql/methods/finitedifferences/meshers/uniform1dmesher.hpp>
#include <ql/methods/finitedifferences/solvers/fdmbackwardsolver.hpp>
#include <ql/methods/finitedifferences/operators/fdmblackscholesop.hpp>
#include <ql/methods/finitedifferences/meshers/fdmblackscholesmesher.hpp>
#include <ql/methods/finitedifferences/utilities/fdminnervaluecalculator.hpp>
#include <ql/methods/finitedifferences/operators/fdmlinearop.hpp>
#include <ql/methods/finitedifferences/operators/fdmlinearoplayout.hpp>
#include <ql/methods/finitedifferences/operators/fdmlinearopcomposite.hpp>
#include <ql/methods/finitedifferences/operators/fdmhestonhullwhiteop.hpp>
#include <ql/methods/finitedifferences/meshers/fdmhestonvariancemesher.hpp>
#include <ql/methods/finitedifferences/operators/fdmhestonop.hpp>
#include <ql/methods/finitedifferences/solvers/fdmhestonsolver.hpp>
#include <ql/methods/finitedifferences/meshers/fdmmeshercomposite.hpp>
#include <ql/methods/finitedifferences/solvers/fdmndimsolver.hpp>
#include <ql/methods/finitedifferences/solvers/fdm3dimsolver.hpp>
#include <ql/methods/finitedifferences/stepconditions/fdmamericanstepcondition.hpp>
#include <ql/methods/finitedifferences/stepconditions/fdmstepconditioncomposite.hpp>
#include <ql/methods/finitedifferences/utilities/fdmdividendhandler.hpp>
#include <ql/methods/finitedifferences/operators/firstderivativeop.hpp>
#include <ql/methods/finitedifferences/operators/secondderivativeop.hpp>
#include <ql/methods/finitedifferences/operators/secondordermixedderivativeop.hpp>
#include <ql/math/matrixutilities/sparseilupreconditioner.hpp>

#if !defined(QL_NO_UBLAS_SUPPORT)
#include <boost/numeric/ublas/vector.hpp>
#include <boost/numeric/ublas/operation.hpp>
#endif

#include <boost/bind.hpp>
#include <numeric>

using namespace QuantLib;
using namespace boost::unit_test_framework;

namespace {

    class FdmHestonExpressCondition : public StepCondition<Array> {
      public:
        FdmHestonExpressCondition(
            const std::vector<Real>& redemptions,
            const std::vector<Real>& triggerLevels,
            const std::vector<Time>& exerciseTimes,
            const boost::shared_ptr<FdmMesher> & mesher)
        : redemptions_(redemptions), triggerLevels_(triggerLevels),
          exerciseTimes_(exerciseTimes), mesher_(mesher) {
        }

        void applyTo(Array& a, Time t) const {
            std::vector<Time>::const_iterator iter
                = std::find(exerciseTimes_.begin(), exerciseTimes_.end(), t);

            if (iter != exerciseTimes_.end()) {
                Size index = std::distance(exerciseTimes_.begin(), iter);

               boost::shared_ptr<FdmLinearOpLayout> layout = mesher_->layout();
                const FdmLinearOpIterator endIter = layout->end();
                for (FdmLinearOpIterator iter = layout->begin();
                     iter != endIter; ++iter) {
                    const Real s = std::exp(mesher_->location(iter, 0));

                    if (s > triggerLevels_[index]) {
                        a[iter.index()] = redemptions_[index];
                    }
                }
            }
        }

      private:
        const std::vector<Real> redemptions_;
        const std::vector<Real> triggerLevels_;
        const std::vector<Time> exerciseTimes_;
        const boost::shared_ptr<FdmMesher> mesher_;
    };

    class ExpressPayoff : public Payoff {
      public:
        std::string name() const { return "ExpressPayoff";}
        std::string description() const { return "ExpressPayoff";}

        Real operator()(Real s) const {
            return  ((s >= 100.0) ? 108.0 : 100.0)
                  - ((s <= 75.0) ? 100.0 - s : 0.0);
        }
    };

    template <class T, class U, class V>
    struct multiplies : public std::binary_function<T, U, V> {
        V operator()(T t, U u) { return t*u;}
    };

}

void FdmLinearOpTest::testFdmLinearOpLayout() {

    BOOST_MESSAGE("Testing indexing of a linear operator...");

    SavedSettings backup;

    Size dims[] = {5,7,8};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    FdmLinearOpLayout layout = FdmLinearOpLayout(dim);

    Size calculatedDim = layout.dim().size();
    Size expectedDim = dim.size();
    if (calculatedDim != expectedDim) {
        BOOST_ERROR("index.dimensions() should be " << expectedDim
                    << ", but is " << calculatedDim);
    }

    Size calculatedSize = layout.size();
    Size expectedSize = std::accumulate(dim.begin(), dim.end(),
                                        1, std::multiplies<Size>());

    if (calculatedSize != expectedSize) {
        BOOST_FAIL("index.size() should be "
                    << expectedSize << ", but is " << calculatedSize);
    }

    for (Size k=0; k < dim[0]; ++k) {
        for (Size l=0; l < dim[1]; ++l) {
            for (Size m=0; m < dim[2]; ++m) {
                std::vector<Size> tmp(3);
                tmp[0] = k; tmp[1] = l; tmp[2] = m;

                Size calculatedIndex = layout.index(tmp);
                Size expectedIndex = k + l*dim[0] + m*dim[0]*dim[1];

                if (expectedIndex != layout.index(tmp)) {
                    BOOST_FAIL("index.size() should be " << expectedIndex
                                <<", but is " << calculatedIndex);
                }
            }
        }
    }

    FdmLinearOpIterator iter = layout.begin();

    for (Size m=0; m < dim[2]; ++m) {
        for (Size l=0; l < dim[1]; ++l) {
            for (Size k=0; k < dim[0]; ++k, ++iter) {
                for (Size n=1; n < 4; ++n) {
                    Size nn = layout.neighbourhood(iter, 1, n);
                    Size calculatedIndex = k + m*dim[0]*dim[1]
                       + ((l < dim[1]-n)? l+n
                                        : dim[1]-1-(l+n-(dim[1]-1)))*dim[0];

                    if (nn != calculatedIndex) {
                        BOOST_FAIL("next neighbourhood index is " << nn
                                    << " but should be " << calculatedIndex);
                    }
                }

                for (Size n=1; n < 7; ++n) {
                    Size nn = layout.neighbourhood(iter, 2, -Integer(n));
                    Size calculatedIndex = k + l*dim[0]
                            + ((m < n) ? n-m : m-n)*dim[0]*dim[1];
                    if (nn != calculatedIndex) {
                        BOOST_FAIL("next neighbourhood index is " << nn
                            << " but should be " << calculatedIndex);
                    }
                }
            }
        }
    }
}

void FdmLinearOpTest::testUniformGridMesher() {

    BOOST_MESSAGE("Testing uniform grid mesher...");

    SavedSettings backup;

    Size dims[] = {5,7,8};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<FdmLinearOpLayout> layout(new FdmLinearOpLayout(dim));
    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>(-5, 10));
    boundaries.push_back(std::pair<Real, Real>( 5, 100));
    boundaries.push_back(std::pair<Real, Real>( 10, 20));

    UniformGridMesher mesher(layout, boundaries);

    const Real dx1 = 15.0/(dim[0]-1);
    const Real dx2 = 95.0/(dim[1]-1);
    const Real dx3 = 10.0/(dim[2]-1);

    Real tol = 100*QL_EPSILON;
    if (   std::fabs(dx1-mesher.dminus(layout->begin(),0)) > tol
        || std::fabs(dx1-mesher.dplus(layout->begin(),0)) > tol
        || std::fabs(dx2-mesher.dminus(layout->begin(),1)) > tol
        || std::fabs(dx2-mesher.dplus(layout->begin(),1)) > tol
        || std::fabs(dx3-mesher.dminus(layout->begin(),2)) > tol
        || std::fabs(dx3-mesher.dplus(layout->begin(),2)) > tol ) {
        BOOST_FAIL("inconsistent uniform mesher object");
    }
}

void FdmLinearOpTest::testFirstDerivativesMapApply() {

    BOOST_MESSAGE("Testing application of first-derivatives map...");

        SavedSettings backup;

    Size dims[] = {400, 100, 50};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));


    boost::shared_ptr<FdmLinearOpLayout> index(new FdmLinearOpLayout(dim));

    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>(-5, 5));
    boundaries.push_back(std::pair<Real, Real>( 0, 10));
    boundaries.push_back(std::pair<Real, Real>( 5, 15));

    boost::shared_ptr<FdmMesher> mesher(
                                 new UniformGridMesher(index, boundaries));

    FirstDerivativeOp map(2, mesher);

    Array r(mesher->layout()->size());
    const FdmLinearOpIterator endIter = index->end();

    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        r[iter.index()] =  std::sin(mesher->location(iter, 0))
                         + std::cos(mesher->location(iter, 2));
    }

    Array t = map.apply(r);
    const Real dz = (boundaries[2].second-boundaries[2].first)/(dims[2]-1);
    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Size z = iter.coordinates()[2];

        const Size z0 = (z > 0) ? z-1 : 1;
        const Size z2 = (z < dims[2]-1) ? z+1 : dims[2]-2;
        const Real lz0 = boundaries[2].first + z0*dz;
        const Real lz2 = boundaries[2].first + z2*dz;

        Real expected;
        if (z == 0) {
            expected = (std::cos(boundaries[2].first+dz)
                        - std::cos(boundaries[2].first))/dz;
        }
        else if (z == dim[2]-1) {
            expected = (std::cos(boundaries[2].second)
                        - std::cos(boundaries[2].second-dz))/dz;
        }
        else {
            expected = (std::cos(lz2)-std::cos(lz0))/(2*dz);
        }

        const Real calculated = t[iter.index()];
        if (std::fabs(calculated - expected) > 1e-10) {
            BOOST_FAIL("first derivative calculation failed."
                        << "\n    calculated: " << calculated
                        << "\n    expected:   " << expected);
        }
    }


}

void FdmLinearOpTest::testSecondDerivativesMapApply() {

    BOOST_MESSAGE("Testing application of second-derivatives map...");

        SavedSettings backup;

        Size dims[] = {50, 50, 50};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<FdmLinearOpLayout> index(new FdmLinearOpLayout(dim));

    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>( 0, 0.5));
    boundaries.push_back(std::pair<Real, Real>( 0, 0.5));
    boundaries.push_back(std::pair<Real, Real>( 0, 0.5));

    boost::shared_ptr<FdmMesher> mesher(
                            new UniformGridMesher(index, boundaries));

    Array r(mesher->layout()->size());
    const FdmLinearOpIterator endIter = index->end();

    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        r[iter.index()] = std::sin(x)*std::cos(y)*std::exp(z);
    }

    Array t = SecondDerivativeOp(0, mesher).apply(r);

    const Real tol = 5e-2;
    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Size i = iter.index();
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        Real d = -std::sin(x)*std::cos(y)*std::exp(z);
        if (iter.coordinates()[0] == 0 || iter.coordinates()[0] == dims[0]-1) {
            d = 0;
        }

        if (std::fabs(d - t[i]) > tol) {
            BOOST_FAIL("numerical derivative in dx^2 deviation is too big"
                << "\n  found at " << x << " " << y << " " << z);
        }
    }

    t = SecondDerivativeOp(1, mesher).apply(r);
    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Size i = iter.index();
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        Real d = -std::sin(x)*std::cos(y)*std::exp(z);
        if (iter.coordinates()[1] == 0 || iter.coordinates()[1] == dims[1]-1) {
            d = 0;
        }

        if (std::fabs(d - t[i]) > tol) {
            BOOST_FAIL("numerical derivative in dy^2 deviation is too big"
                << "\n  found at " << x << " " << y << " " << z);
        }
    }

    t = SecondDerivativeOp(2, mesher).apply(r);
    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Size i = iter.index();
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        Real d = std::sin(x)*std::cos(y)*std::exp(z);
        if (iter.coordinates()[2] == 0 || iter.coordinates()[2] == dims[2]-1) {
            d = 0;
        }

        if (std::fabs(d - t[i]) > tol) {
            BOOST_FAIL("numerical derivative in dz^2 deviation is too big"
                << "\n  found at " << x << " " << y << " " << z);
        }
    }


}

void FdmLinearOpTest::testSecondOrderMixedDerivativesMapApply() {

    BOOST_MESSAGE(
        "Testing application of second-order mixed-derivatives map...");

        SavedSettings backup;

    Size dims[] = {50, 50, 50};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<FdmLinearOpLayout> index(new FdmLinearOpLayout(dim));

    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>( 0, 0.5));
    boundaries.push_back(std::pair<Real, Real>( 0, 0.5));
    boundaries.push_back(std::pair<Real, Real>( 0, 0.5));

    boost::shared_ptr<FdmMesher> mesher(
        new UniformGridMesher(index, boundaries));

    Array r(mesher->layout()->size());
    const FdmLinearOpIterator endIter = index->end();

    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        r[iter.index()] = std::sin(x)*std::cos(y)*std::exp(z);
    }

    Array t = SecondOrderMixedDerivativeOp(0, 1, mesher).apply(r);
    Array u = SecondOrderMixedDerivativeOp(1, 0, mesher).apply(r);

    const Real tol = 5e-2;
    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Size i = iter.index();
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        const Real d = -std::cos(x)*std::sin(y)*std::exp(z);

        if (std::fabs(d - t[i]) > tol) {
            BOOST_FAIL("numerical derivative in dxdy deviation is too big"
                        << "\n  found at " << x << " " << y << " " << z);
        }

        if (std::fabs(t[i]-u[i]) > 1e5*QL_EPSILON) {
            BOOST_FAIL("numerical derivative in dxdy not equal to dydx"
                    << "\n  found at " << x << " " << y << " " << z
                    << "\n  value    " << std::fabs(t[i]-u[i]));
        }
    }

    t = SecondOrderMixedDerivativeOp(0, 2, mesher).apply(r);
    u = SecondOrderMixedDerivativeOp(2, 0, mesher).apply(r);
    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Size i = iter.index();
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        const Real d = std::cos(x)*std::cos(y)*std::exp(z);

        if (std::fabs(d - t[i]) > tol) {
            BOOST_FAIL("numerical derivative in dxdy deviation is too big"
                << "\n  found at " << x << " " << y << " " << z);
        }

        if (std::fabs(t[i]-u[i]) > 1e5*QL_EPSILON) {
            BOOST_FAIL("numerical derivative in dxdz not equal to dzdx"
                << "\n  found at " << x << " " << y << " " << z
                << "\n  value    " << std::fabs(t[i]-u[i]));
        }
    }

    t = SecondOrderMixedDerivativeOp(1, 2, mesher).apply(r);
    u = SecondOrderMixedDerivativeOp(2, 1, mesher).apply(r);
    for (FdmLinearOpIterator iter = index->begin(); iter != endIter; ++iter) {
        const Size i = iter.index();
        const Real x = mesher->location(iter, 0);
        const Real y = mesher->location(iter, 1);
        const Real z = mesher->location(iter, 2);

        const Real d = -std::sin(x)*std::sin(y)*std::exp(z);

        if (std::fabs(d - t[i]) > tol) {
            BOOST_FAIL("numerical derivative in dydz deviation is too big"
                << "\n  found at " << x << " " << y << " " << z);
        }

        if (std::fabs(t[i]-u[i]) > 1e5*QL_EPSILON) {
            BOOST_FAIL("numerical derivative in dydz not equal to dzdy"
                << "\n  found at " << x << " " << y << " " << z
                << "\n  value    " << std::fabs(t[i]-u[i]));
        }
    }


}

void FdmLinearOpTest::testTripleBandMapSolve() {

    BOOST_MESSAGE("Testing triple-band map solution...");

    SavedSettings backup;

    Size dims[] = {100, 400};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<FdmLinearOpLayout> layout(new FdmLinearOpLayout(dim));

    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>( 0, 1.0));
    boundaries.push_back(std::pair<Real, Real>( 0, 1.0));

    boost::shared_ptr<FdmMesher> mesher(
        new UniformGridMesher(layout, boundaries));

    FirstDerivativeOp dy(1, mesher);
    dy.axpyb(Array(1, 2.0), dy, dy, Array(1, 1.0));

    // check copy constructor
    FirstDerivativeOp copyOfDy(dy);

    Array u(layout->size());
    for (Size i=0; i < layout->size(); ++i)
        u[i] = std::sin(0.1*i)+std::cos(0.35*i);

    Array t(dy.solve_splitting(copyOfDy.apply(u), 1.0, 0.0));
    for (Size i=0; i < u.size(); ++i) {
        if (std::fabs(u[i] - t[i]) > 1e-6) {
            BOOST_FAIL("solve and apply are not consistent "
                    << "\n expected      : " << u[i]
                    << "\n calculated    : " << t[i]);
        }
    }

    FirstDerivativeOp dx(0, mesher);
    dx.axpyb(Array(), dx, dx, Array(1, 1.0));

    FirstDerivativeOp copyOfDx(0, mesher);
    // check assignment
    copyOfDx = dx;

    t = dx.solve_splitting(copyOfDx.apply(u), 1.0, 0.0);
    for (Size i=0; i < u.size(); ++i) {
        if (std::fabs(u[i] - t[i]) > 1e-6) {
            BOOST_FAIL("solve and apply are not consistent "
                << "\n expected      : " << u[i]
                << "\n calculated    : " << t[i]);
        }
    }

    SecondDerivativeOp dxx(0, mesher);
    dxx.axpyb(Array(1, 0.5), dxx, dx, Array(1, 1.0));

    // check of copy constructor
    SecondDerivativeOp copyOfDxx(dxx);

    t = dxx.solve_splitting(copyOfDxx.apply(u), 1.0, 0.0);

    for (Size i=0; i < u.size(); ++i) {
        if (std::fabs(u[i] - t[i]) > 1e-6) {
            BOOST_FAIL("solve and apply are not consistent "
                << "\n expected      : " << u[i]
                << "\n calculated    : " << t[i]);
        }
    }

    //check assigment operator
    copyOfDxx = SecondDerivativeOp(1, mesher);
    copyOfDxx = dxx;

    t = dxx.solve_splitting(copyOfDxx.apply(u), 1.0, 0.0);

    for (Size i=0; i < u.size(); ++i) {
        if (std::fabs(u[i] - t[i]) > 1e-6) {
            BOOST_FAIL("solve and apply are not consistent "
                << "\n expected      : " << u[i]
                << "\n calculated    : " << t[i]);
        }
    }
}


void FdmLinearOpTest::testFdmHestonBarrier() {

    BOOST_MESSAGE("Testing FDM with Barrier option in Heston model...");

    SavedSettings backup;

    Size dims[] = {200, 100};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<FdmLinearOpLayout> index(new FdmLinearOpLayout(dim));

    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>( 3.8, 4.905274778));
    boundaries.push_back(std::pair<Real, Real>( 0.000, 1.0));

    boost::shared_ptr<FdmMesher> mesher(
        new UniformGridMesher(index, boundaries));

    Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(100.0)));

    Handle<YieldTermStructure> rTS(flatRate(0.05, Actual365Fixed()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual365Fixed()));

    boost::shared_ptr<HestonProcess> hestonProcess(
        new HestonProcess(rTS, qTS, s0, 0.04, 2.5, 0.04, 0.66, -0.8));

    Settings::instance().evaluationDate() = Date(28, March, 2004);
    Date exerciseDate(28, March, 2005);

    boost::shared_ptr<FdmLinearOpComposite> hestonOp(
                                   new FdmHestonOp(mesher, hestonProcess));

    Array rhs(mesher->layout()->size());
    const FdmLinearOpIterator endIter = mesher->layout()->end();
    for (FdmLinearOpIterator iter = mesher->layout()->begin();
         iter != endIter; ++iter) {
        rhs[iter.index()]=std::max(std::exp(mesher->location(iter,0))-100, 0.0);
    }

    FdmBoundaryConditionSet bcSet;
    bcSet.push_back(boost::shared_ptr<FdmDirichletBoundary>(
        new FdmDirichletBoundary(mesher, 0.0, 0,
                                 FdmDirichletBoundary::Upper)));

    const Real theta=0.5+std::sqrt(3.0)/6.;
    HundsdorferScheme hsEvolver(theta, 0.5, hestonOp, bcSet);
    FiniteDifferenceModel<HundsdorferScheme> hsModel(hsEvolver);
    hsModel.rollback(rhs, 1.0, 0.0, 50);

    Matrix ret(dims[0], dims[1]);
    for (Size i=0; i < dims[0]; ++i)
        for (Size j=0; j < dims[1]; ++j)
            ret[i][j] = rhs[i+j*dims[0]];

    std::vector<Real> tx, ty;
    for (FdmLinearOpIterator iter = mesher->layout()->begin();
        iter != endIter; ++iter) {
            if (iter.coordinates()[1] == 0) {
                tx.push_back(mesher->location(iter, 0));
            }
            if (iter.coordinates()[0] == 0) {
                ty.push_back(mesher->location(iter, 1));
            }
    }

    BilinearInterpolation interpolate(ty.begin(), ty.end(),
                                      tx.begin(), tx.end(), ret);

    const Real x = 100;
    const Real v0 = 0.04;

    const Real npv = interpolate(v0, std::log(x));
    const Real delta = 0.5*(
            interpolate(v0, std::log(x+1))-interpolate(v0, std::log(x-1)));
    const Real gamma =   interpolate(v0, std::log(x+1))
                      +  interpolate(v0, std::log(x-1)) - 2*npv;

    const Real npvExpected   = 9.049016;
    const Real deltaExpected = 0.511285;
    const Real gammaExpected = -0.034296;

    if (std::fabs(npv - npvExpected) > 0.000001) {
        BOOST_FAIL("Error in calculating PV for Heston barrier option");
    }

    if (std::fabs(delta - deltaExpected) > 0.000001) {
        BOOST_FAIL("Error in calculating Delta for Heston barrier option");
    }

    if (std::fabs(gamma - gammaExpected) > 0.000001) {
            BOOST_FAIL("Error in calculating Gamma for Heston barrier option");
    }
}

void FdmLinearOpTest::testFdmHestonAmerican() {

    BOOST_MESSAGE("Testing FDM with American option in Heston model...");

    SavedSettings backup;

    Size dims[] = {200, 100};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<FdmLinearOpLayout> index(new FdmLinearOpLayout(dim));

    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>( 3.8, std::log(220.0)));
    boundaries.push_back(std::pair<Real, Real>( 0.000, 1.0));

    boost::shared_ptr<FdmMesher> mesher(
        new UniformGridMesher(index, boundaries));

    Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(100.0)));

    Handle<YieldTermStructure> rTS(flatRate(0.05, Actual365Fixed()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual365Fixed()));

    boost::shared_ptr<HestonProcess> hestonProcess(
        new HestonProcess(rTS, qTS, s0, 0.04, 2.5, 0.04, 0.66, -0.8));

    Settings::instance().evaluationDate() = Date(28, March, 2004);
    Date exerciseDate(28, March, 2005);

    boost::shared_ptr<FdmLinearOpComposite> LinearOp(
        new FdmHestonOp(mesher, hestonProcess));

    boost::shared_ptr<Payoff> payoff(new PlainVanillaPayoff(Option::Put, 100.0));
    Array rhs(mesher->layout()->size());
    const FdmLinearOpIterator endIter = mesher->layout()->end();
    for (FdmLinearOpIterator iter = mesher->layout()->begin();
        iter != endIter; ++iter) {
            rhs[iter.index()]
                = payoff->operator ()(std::exp(mesher->location(iter, 0)));
    }

    FdmAmericanStepCondition condition(mesher,
        boost::shared_ptr<FdmInnerValueCalculator>(
                                     new FdmLogInnerValue(payoff, mesher, 0)));
    const Real theta=0.5+std::sqrt(3.0)/6.;
    HundsdorferScheme hsEvolver(theta, 0.5, LinearOp);
    FiniteDifferenceModel<HundsdorferScheme> hsModel(hsEvolver);
    hsModel.rollback(rhs, 1.0, 0.0, 50, condition);

    Matrix ret(dims[0], dims[1]);
    for (Size i=0; i < dims[0]; ++i)
        for (Size j=0; j < dims[1]; ++j)
            ret[i][j] = rhs[i+j*dims[0]];

    std::vector<Real> tx, ty;
    for (FdmLinearOpIterator iter = mesher->layout()->begin();
        iter != endIter; ++iter) {
            if (iter.coordinates()[1] == 0) {
                tx.push_back(mesher->location(iter, 0));
            }
            if (iter.coordinates()[0] == 0) {
                ty.push_back(mesher->location(iter, 1));
            }
    }

    BilinearInterpolation interpolate(ty.begin(), ty.end(),
                                      tx.begin(), tx.end(), ret);

    const Real x = 100;
    const Real v0 = 0.04;

    const Real npv = interpolate(v0, std::log(x));
    const Real npvExpected = 5.641648;

    if (std::fabs(npv - npvExpected) > 0.000001) {
        BOOST_FAIL("Error in calculating PV for Heston American Option");
    }
}

void FdmLinearOpTest::testFdmHestonExpress() {

    BOOST_MESSAGE("Testing FDM with express certificate in Heston model...");

    SavedSettings backup;

    Size dims[] = {200, 100};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<FdmLinearOpLayout> index(new FdmLinearOpLayout(dim));

    std::vector<std::pair<Real, Real> > boundaries;
    boundaries.push_back(std::pair<Real, Real>(3.8, std::log(220.0)));
    boundaries.push_back(std::pair<Real, Real>(0.000, 1.0));

    boost::shared_ptr<FdmMesher> mesher(
                            new UniformGridMesher(index, boundaries));

    Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(100.0)));

    Handle<YieldTermStructure> rTS(flatRate(0.05, Actual365Fixed()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual365Fixed()));

    Handle<HestonProcess> hestonProcess(boost::shared_ptr<HestonProcess> (
        new HestonProcess(rTS, qTS, s0, 0.04, 2.5, 0.04, 0.66, -0.8)));

    const Date exerciseDate(28, March, 2005);
    const Date evaluationDate(28, March, 2004);
    Settings::instance().evaluationDate() = evaluationDate;

    std::vector<Real> triggerLevels(2);
    triggerLevels[0] = triggerLevels[1] = 100.0;
    std::vector<Real> redemptions(2);
    redemptions[0] = redemptions[1] = 108.0;
    std::vector<Time> exerciseTimes(2);
    exerciseTimes[0] = 0.333; exerciseTimes[1] = 0.666;

    DividendSchedule dividendSchedule(1, boost::shared_ptr<Dividend>(
        new FixedDividend(2.5, evaluationDate + Period(6, Months))));
    boost::shared_ptr<FdmDividendHandler> dividendCondition(
        new FdmDividendHandler(dividendSchedule, mesher,
                               rTS->referenceDate(),
                               rTS->dayCounter(), 0));

    boost::shared_ptr<StepCondition<Array> > expressCondition(
        new FdmHestonExpressCondition(redemptions, triggerLevels,
                                      exerciseTimes, mesher));

    std::list<std::vector<Time> > stoppingTimes;
    stoppingTimes.push_back(exerciseTimes);
    stoppingTimes.push_back(dividendCondition->dividendTimes());

    std::list<boost::shared_ptr<StepCondition<Array> > > conditions;
    conditions.push_back(expressCondition);
    conditions.push_back(dividendCondition);

    boost::shared_ptr<FdmStepConditionComposite> condition(
        new FdmStepConditionComposite(stoppingTimes, conditions));

    boost::shared_ptr<Payoff> payoff(new ExpressPayoff());

    boost::shared_ptr<FdmInnerValueCalculator> calculator(
                                    new FdmLogInnerValue(payoff, mesher, 0));

    const FdmBoundaryConditionSet bcSet;
    const FdmSolverDesc solverDesc = { mesher, bcSet,
                                       condition, calculator, 1.0, 50, 0 };
    FdmHestonSolver solver(hestonProcess, solverDesc);

    const Real s = s0->value();
    const Real v0 = 0.04;

    if (std::fabs(solver.valueAt(s, v0) - 101.027) > 0.01) {
        BOOST_FAIL("Error in calculating PV for Heston Express Certificate");
    }

    if (std::fabs(solver.deltaAt(s, v0) - 0.4181) > 0.001) {
        BOOST_FAIL("Error in calculating Delta for Heston Express Certificate");
    }

    if (std::fabs(solver.gammaAt(s, v0) + 0.0400) > 0.001) {
        BOOST_FAIL("Error in calculating Gamma for Heston Express Certificate");
    }

    if (std::fabs(solver.meanVarianceDeltaAt(s, v0) - 0.6602) > 0.001) {
        BOOST_FAIL("Error in calculating mean variance Delta for "
                "Heston Express Certificate");
    }

    if (std::fabs(solver.meanVarianceGammaAt(s, v0) + 0.0316) > 0.001) {
        BOOST_FAIL("Error in calculating mean variance Delta for "
                "Heston Express Certificate");
    }
}


namespace {

    boost::shared_ptr<HybridHestonHullWhiteProcess> createHestonHullWhite(
        Time maturity) {

        DayCounter dc = Actual365Fixed();
        const Date today = Settings::instance().evaluationDate();
        Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(100.0)));

        std::vector<Date> dates;
        std::vector<Rate> rates, divRates;

        for (Size i=0; i <= 25; ++i) {
            dates.push_back(today+Period(i, Years));
            rates.push_back(0.05);
            divRates.push_back(0.02);
        }

        const Handle<YieldTermStructure> rTS(
           boost::shared_ptr<YieldTermStructure>(new ZeroCurve(dates, rates, dc)));
        const Handle<YieldTermStructure> qTS(
           boost::shared_ptr<YieldTermStructure>(
                                              new ZeroCurve(dates, divRates, dc)));

        const Real v0 = 0.04;
        boost::shared_ptr<HestonProcess> hestonProcess(
            new HestonProcess(rTS, qTS, s0, v0, 1.0, v0*0.75, 0.4, -0.7));

        boost::shared_ptr<HullWhiteForwardProcess> hwFwdProcess(
                            new HullWhiteForwardProcess(rTS, 0.00883, 0.01));
        hwFwdProcess->setForwardMeasureTime(maturity);

        const Real equityShortRateCorr = -0.7;

        return boost::shared_ptr<HybridHestonHullWhiteProcess>(
                new HybridHestonHullWhiteProcess(hestonProcess, hwFwdProcess,
                                                 equityShortRateCorr));
    }

    FdmSolverDesc createSolverDesc(
        const std::vector<Size>& dim,
        const boost::shared_ptr<HybridHestonHullWhiteProcess>& process) {

        const Time maturity
                    = process->hullWhiteProcess()->getForwardMeasureTime();

        boost::shared_ptr<FdmLinearOpLayout> layout(new FdmLinearOpLayout(dim));

        std::vector<boost::shared_ptr<Fdm1dMesher> > mesher1d;
        mesher1d.push_back(boost::shared_ptr<Fdm1dMesher>(
                new Uniform1dMesher(std::log(22.0), std::log(440.0), dim[0])));
        mesher1d.push_back(boost::shared_ptr<Fdm1dMesher>(
                new FdmHestonVarianceMesher(dim[1], process->hestonProcess(),
                                            maturity)));
        mesher1d.push_back(boost::shared_ptr<Fdm1dMesher>(
                new Uniform1dMesher(-0.15, 0.15, dim[2])));

        const boost::shared_ptr<FdmMesher> mesher(
            new FdmMesherComposite(mesher1d));

        const FdmBoundaryConditionSet boundaries;

        std::list<std::vector<Time> > stoppingTimes;
        std::list<boost::shared_ptr<StepCondition<Array> > > stepConditions;

        boost::shared_ptr<FdmStepConditionComposite> conditions(
            new FdmStepConditionComposite(
                                     std::list<std::vector<Time> >(),
                                     FdmStepConditionComposite::Conditions()));

        boost::shared_ptr<StrikedTypePayoff> payoff(
                                  new PlainVanillaPayoff(Option::Call, 160.0));

        boost::shared_ptr<FdmInnerValueCalculator> calculator(
                                       new FdmLogInnerValue(payoff, mesher, 0));

        const Size tGrid = 100;
        const Size dampingSteps = 0;

        FdmSolverDesc desc = { mesher, boundaries,
                               conditions, calculator,
                               maturity, tGrid, dampingSteps };

        return desc;
    }
}

void FdmLinearOpTest::testFdmHestonHullWhiteOp() {
    BOOST_MESSAGE("Testing FDM with Heston Hull-White model...");

    SavedSettings backup;

    const Date today = Date(28, March, 2004);
    Settings::instance().evaluationDate() = today;

    Date exerciseDate(28, March, 2012);
    const Time maturity = Actual365Fixed().yearFraction(today, exerciseDate);

    Size dims[] = {51, 31, 31};
    const std::vector<Size> dim(dims, dims+LENGTH(dims));

    boost::shared_ptr<HybridHestonHullWhiteProcess> jointProcess
                                            = createHestonHullWhite(maturity);
    FdmSolverDesc desc = createSolverDesc(dim, jointProcess);
    boost::shared_ptr<FdmMesher> mesher = desc.mesher;

    boost::shared_ptr<HullWhiteForwardProcess> hwFwdProcess
                                            = jointProcess->hullWhiteProcess();

    boost::shared_ptr<HullWhiteProcess> hwProcess(
        new HullWhiteProcess(jointProcess->hestonProcess()->riskFreeRate(),
                             hwFwdProcess->a(), hwFwdProcess->sigma()));

    boost::shared_ptr<FdmLinearOpComposite> linearOp(
        new FdmHestonHullWhiteOp(mesher,
                                 jointProcess->hestonProcess(),
                                 hwProcess,
                                 jointProcess->eta()));

    Array rhs(mesher->layout()->size());
    const FdmLinearOpIterator endIter = mesher->layout()->end();
    for (FdmLinearOpIterator iter = mesher->layout()->begin();
        iter != endIter; ++iter) {
            rhs[iter.index()] = desc.calculator->avgInnerValue(iter, maturity);
    }

    const Real theta = 0.5+std::sqrt(3.0)/6.;
    HundsdorferScheme hsEvolver(theta, 0.5, linearOp);
    FiniteDifferenceModel<HundsdorferScheme> hsModel(hsEvolver);

    hsModel.rollback(rhs, maturity, 0.0, desc.timeSteps);

    std::vector<Real> tx, ty, tr, y;
    for (FdmLinearOpIterator iter = mesher->layout()->begin();
        iter != endIter; ++iter) {
            if (iter.coordinates()[1] == 0 && iter.coordinates()[2] == 0) {
                tx.push_back(mesher->location(iter, 0));
            }
            if (iter.coordinates()[0] == 0 && iter.coordinates()[2] == 0) {
                ty.push_back(mesher->location(iter, 1));
            }
            if (iter.coordinates()[0] == 0 && iter.coordinates()[1] == 0) {
                tr.push_back(mesher->location(iter, 2));
            }
    }

    const Real x0 = 100;
    const Real v0 = jointProcess->hestonProcess()->v0();
    const Real r0 = 0;
    for (Size k=0; k < dim[2]; ++k) {
        Matrix ret(dim[0], dim[1]);
        for (Size i=0; i < dim[0]; ++i)
            for (Size j=0; j < dim[1]; ++j)
                ret[i][j] = rhs[ i+j*dim[0]+k*dim[0]*dim[1] ];

        y.push_back(BicubicSpline(ty.begin(), ty.end(),
                                  tx.begin(), tx.end(), ret)(v0, std::log(x0)));
    }

    const Real directCalc
        = MonotonicCubicNaturalSpline(tr.begin(), tr.end(), y.begin())(r0);

    std::vector<Real> x(3);
    x[0] = std::log(x0); x[1] = v0; x[2] = r0;

    Fdm3DimSolver solver3d(desc, FdmSchemeDesc::Hundsdorfer(), linearOp);
    const Real solverCalc = solver3d.interpolateAt(x[0], x[1], x[2]);
    const Real solverTheta = solver3d.thetaAt(x[0], x[1], x[2]);

    if (std::fabs(directCalc - solverCalc) > 1e-4) {
        BOOST_FAIL("Error in calculating PV for Heston Hull White Option");
    }


    FdmNdimSolver<3> solverNd(desc, FdmSchemeDesc::Hundsdorfer(), linearOp);
    const Real solverNdCalc = solverNd.interpolateAt(x);
    const Real solverNdTheta = solverNd.thetaAt(x);

    if (std::fabs(solverNdCalc - solverCalc) > 1e-4) {
        BOOST_FAIL("Error in calculating PV for Heston Hull White Option");
    }
    if (std::fabs(solverNdTheta - solverTheta) > 1e-4) {
        BOOST_FAIL("Error in calculating PV for Heston Hull White Option");
    }

    VanillaOption option(
            boost::shared_ptr<StrikedTypePayoff>(
                                new PlainVanillaPayoff(Option::Call, 160.0)),
            boost::shared_ptr<Exercise>(new EuropeanExercise(exerciseDate)));

    const Real tol = 0.025;
    option.setPricingEngine(
            MakeMCHestonHullWhiteEngine<PseudoRandom>(jointProcess)
            .withSteps(200)
            .withAntitheticVariate()
            .withControlVariate()
            .withAbsoluteTolerance(tol)
            .withSeed(42));

    // the following takes far too long
    // const Real expected = option.NPV();
    // use precalculated value instead
    const Real expected = 4.73;

    if (std::fabs(directCalc - expected) > 3*tol) {
        BOOST_FAIL("Error in calculating MC PV for Heston Hull White Option");
    }
}

#if !defined(QL_NO_UBLAS_SUPPORT)
namespace {
    Disposable<Array> axpy(
        const boost::numeric::ublas::compressed_matrix<Real>& A,
        const Array& x) {
        
        boost::numeric::ublas::vector<Real> tmpX(x.size()), y(x.size());
        std::copy(x.begin(), x.end(), tmpX.begin());
        boost::numeric::ublas::axpy_prod(A, tmpX, y);

        Array retVal(y.begin(), y.end());
        return retVal;
    }
}
#endif


void FdmLinearOpTest::testBiCGstab() {
#if !defined(QL_NO_UBLAS_SUPPORT)
    BOOST_MESSAGE("Testing BiCGstab with Heston operator...");

    SavedSettings backup;
    
    const Size n=41, m=21;
    const Real theta = 1.0;
    boost::numeric::ublas::compressed_matrix<Real> a(n*m, n*m);
    
    for (Size i=0; i < n; ++i) {
        for (Size j=0; j < m; ++j) {
            const Size k = i*m+j;
            a(k,k)=1.0;

            if (i > 0 && j > 0 && i <n-1 && j < m-1) {
                const Size im1 = i-1;
                const Size ip1 = i+1;
                const Size jm1 = j-1;
                const Size jp1 = j+1;
                const Real delta = theta/((ip1-im1)*(jp1-jm1));

                a(k,im1*m+jm1) =  delta;
                a(k,im1*m+jp1) = -delta;
                a(k,ip1*m+jm1) = -delta;
                a(k,ip1*m+jp1) =  delta;
            }
        }
    }
    
    boost::function<Disposable<Array>(const Array&)> matmult(
                                                    boost::bind(&axpy, a, _1));
    
    SparseILUPreconditioner ilu(a, 4);
    boost::function<Disposable<Array>(const Array&)> precond(
         boost::bind(&SparseILUPreconditioner::apply, &ilu, _1));
    
    Array b(n*m);
    MersenneTwisterUniformRng rng(1234);
    for (Size i=0; i < b.size(); ++i) {
        b[i] = rng.next().value;
    }

    const Real tol = 1e-10;

    const BiCGstab biCGstab(matmult, n*m, tol, precond);
    const Array x = biCGstab.solve(b).x;

    const Real error = std::sqrt(DotProduct(b-axpy(a, x), 
                                 b-axpy(a, x))/DotProduct(b,b));

    if (error > tol) {
        BOOST_FAIL("Error calculating the inverse using BiCGstab" <<
                "\n tolerance:  " << tol <<
                "\n error:      " << error);
    }  
#endif
}

void FdmLinearOpTest::testCrankNicolsonWithDamping() {

    BOOST_MESSAGE("Testing Crank-Nicolson with initial implicit damping steps "
                  "for a digital option...");

    SavedSettings backup;

    DayCounter dc = Actual360();
    Date today = Date::todaysDate();

    boost::shared_ptr<SimpleQuote> spot(new SimpleQuote(100.0));
    boost::shared_ptr<YieldTermStructure> qTS = flatRate(today, 0.06, dc);
    boost::shared_ptr<YieldTermStructure> rTS = flatRate(today, 0.06, dc);
    boost::shared_ptr<BlackVolTermStructure> volTS = flatVol(today, 0.35, dc);

    boost::shared_ptr<StrikedTypePayoff> payoff(
                             new CashOrNothingPayoff(Option::Put, 100, 10.0));

    Time maturity = 0.75;
    Date exDate = today + Integer(maturity*360+0.5);
    boost::shared_ptr<Exercise> exercise(new EuropeanExercise(exDate));

    boost::shared_ptr<BlackScholesMertonProcess> process(new
        BlackScholesMertonProcess(Handle<Quote>(spot),
                                  Handle<YieldTermStructure>(qTS),
                                  Handle<YieldTermStructure>(rTS),
                                  Handle<BlackVolTermStructure>(volTS)));
    boost::shared_ptr<PricingEngine> engine(
                                new AnalyticEuropeanEngine(process));

    VanillaOption opt(payoff, exercise);
    opt.setPricingEngine(engine);
    Real expectedPV = opt.NPV();
    Real expectedGamma = opt.gamma();

    // fd pricing using implicit damping steps and Crank Nicolson
    const Size csSteps = 25, dampingSteps = 3, xGrid = 400;
    const std::vector<Size> dim(1, xGrid);

    boost::shared_ptr<FdmLinearOpLayout> layout(new FdmLinearOpLayout(dim));
    const boost::shared_ptr<Fdm1dMesher> equityMesher(
        new FdmBlackScholesMesher(
                dim[0], process, maturity, payoff->strike(),
                Null<Real>(), Null<Real>(), 0.0001, 1.5,
                std::pair<Real, Real>(payoff->strike(), 0.01)));

    const boost::shared_ptr<FdmMesher> mesher (
        new FdmMesherComposite(equityMesher));

    boost::shared_ptr<FdmBlackScholesOp> map(
                     new FdmBlackScholesOp(mesher, process, payoff->strike()));

    boost::shared_ptr<FdmInnerValueCalculator> calculator(
                                  new FdmLogInnerValue(payoff, mesher, 0));

    Array rhs(layout->size()), x(layout->size());
    const FdmLinearOpIterator endIter = layout->end();

    for (FdmLinearOpIterator iter = layout->begin(); iter != endIter;
         ++iter) {
        rhs[iter.index()] = calculator->avgInnerValue(iter, maturity);
        x[iter.index()] = mesher->location(iter, 0);
    }

    FdmBackwardSolver solver(map, FdmBoundaryConditionSet(), 
                             boost::shared_ptr<FdmStepConditionComposite>(),
                             FdmSchemeDesc::Douglas());
    solver.rollback(rhs, maturity, 0.0, csSteps, dampingSteps);

    MonotonicCubicNaturalSpline spline(x.begin(), x.end(), rhs.begin());

    Real s = spot->value();
    Real calculatedPV = spline(std::log(s));
    Real calculatedGamma = (spline.secondDerivative(std::log(s))
                            - spline.derivative(std::log(s))    )/(s*s);

    Real relTol = 2e-3;

    if (std::fabs(calculatedPV - expectedPV) > relTol*expectedPV) {
        BOOST_FAIL("Error calculating the PV of the digital option" <<
                "\n rel. tolerance:  " << relTol <<
                "\n expected:        " << expectedPV <<
                "\n calculated:      " << calculatedPV);
    }
    if (std::fabs(calculatedGamma - expectedGamma) > relTol*expectedGamma) {
        BOOST_FAIL("Error calculating the Gamma of the digital option" <<
                "\n rel. tolerance:  " << relTol <<
                "\n expected:        " << expectedGamma <<
                "\n calculated:      " << calculatedGamma);
    }
}

void FdmLinearOpTest::testSpareMatrixReference() {
#ifndef QL_NO_UBLAS_SUPPORT
    BOOST_MESSAGE("Testing SparseMatrixReference type...");

    const Size rows    = 10;
    const Size columns = 10;
    const Size nMatrices = 5;
    const Size nElements = 50;

    PseudoRandom::urng_type rng(1234ul);

    SparseMatrix expected(rows, columns);
    std::vector<SparseMatrix> v(nMatrices, SparseMatrix(rows, columns));
    std::vector<SparseMatrixReference> refs;

    for (Size i=0; i < v.size(); ++i) {
        SparseMatrixReference m(v[i]);
        for (Size j=0; j < nElements; ++j) {
            const Size row    = rng.next().value*rows;
            const Size column = rng.next().value*columns;

            const Real value = rng.next().value;
            m(row, column)        += value;
            expected(row, column) += value;
        }

        refs.push_back(m);
    }

    SparseMatrix calculated = std::accumulate(refs.begin()+1, refs.end(),
                                              SparseMatrix(refs.front()));

    for (Size i=0; i < rows; ++i) {
        for (Size j=0; j < columns; ++j) {
            if (std::fabs(calculated(i,j) - expected(i,j)) > 100*QL_EPSILON) {
                BOOST_FAIL("Error using sparse matrix references in " <<
                           "Element (" << i << ", " << j << ")" <<
                        "\n expected  : " << expected(i, j) <<
                        "\n calculated: " << calculated(i, j));
            }
        }
    }
#endif
}

test_suite* FdmLinearOpTest::suite() {
    test_suite* suite = BOOST_TEST_SUITE("linear operator tests");

    suite->add(
        QUANTLIB_TEST_CASE(&FdmLinearOpTest::testFdmLinearOpLayout));
    suite->add(
        QUANTLIB_TEST_CASE(&FdmLinearOpTest::testUniformGridMesher));
    suite->add(
        QUANTLIB_TEST_CASE(&FdmLinearOpTest::testFirstDerivativesMapApply));
    suite->add(
        QUANTLIB_TEST_CASE(&FdmLinearOpTest::testSecondDerivativesMapApply));
    suite->add(QUANTLIB_TEST_CASE(
            &FdmLinearOpTest::testSecondOrderMixedDerivativesMapApply));
    suite->add(
        QUANTLIB_TEST_CASE(&FdmLinearOpTest::testTripleBandMapSolve));
    suite->add(QUANTLIB_TEST_CASE(&FdmLinearOpTest::testFdmHestonBarrier));
    suite->add(QUANTLIB_TEST_CASE(&FdmLinearOpTest::testFdmHestonAmerican));
    suite->add(QUANTLIB_TEST_CASE(&FdmLinearOpTest::testFdmHestonExpress));
    suite->add(QUANTLIB_TEST_CASE(&FdmLinearOpTest::testFdmHestonHullWhiteOp));
    suite->add(QUANTLIB_TEST_CASE(&FdmLinearOpTest::testBiCGstab));
    suite->add(
        QUANTLIB_TEST_CASE(&FdmLinearOpTest::testCrankNicolsonWithDamping));
#ifndef QL_NO_UBLAS_SUPPORT
      suite->add(
          QUANTLIB_TEST_CASE(&FdmLinearOpTest::testSpareMatrixReference));
#endif


    return suite;
    
}
