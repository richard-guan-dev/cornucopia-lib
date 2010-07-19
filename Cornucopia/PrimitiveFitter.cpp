/*--
    PrimitiveFitter.cpp  

    This file is part of the Cornucopia curve sketching library.
    Copyright (C) 2010 Ilya Baran (ibaran@mit.edu)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "PrimitiveFitter.h"
#include "Resampler.h"
#include "Fitter.h"
#include "Polyline.h"
#include "PrimitiveFitUtils.h"
#include "ErrorComputer.h"
#include "Solver.h"

using namespace std;
using namespace Eigen;
NAMESPACE_Cornu

class OneCurveProblem : public LSProblem
{
public:
    OneCurveProblem(const FitPrimitive &primitive, ErrorComputerConstPtr errorComputer)
        : _primitive(primitive), _errorComputer(errorComputer)  {}

    //overrides
    double error(const VectorXd &x)
    {
        setParams(x);
        return _errorComputer->computeError(_primitive.curve, _primitive.startIdx, _primitive.endIdx);
    }

    LSEvalData *createEvalData()
    {
        return new LSDenseEvalData();
    }
    void eval(const VectorXd &x, LSEvalData *data)
    {
        LSDenseEvalData *curveData = static_cast<LSDenseEvalData *>(data);
        setParams(x);
        MatrixXd &errDer = curveData->errDerRef();
        _errorComputer->computeErrorVector(_primitive.curve, _primitive.startIdx, _primitive.endIdx,
                                           curveData->errVectorRef(), &errDer);

        if(_primitive.curve->getType() == CurvePrimitive::CLOTHOID)
        {
            double invLength = 1. / x(CurvePrimitive::LENGTH);
            errDer.col(CurvePrimitive::DCURVATURE) *= invLength;

            //compute the derivative with respect to curvature and length as well.
            errDer.col(CurvePrimitive::CURVATURE) -= errDer.col(CurvePrimitive::DCURVATURE);
            double dcurvature = (x(CurvePrimitive::DCURVATURE) - x(CurvePrimitive::CURVATURE)) * invLength;
            errDer.col(CurvePrimitive::LENGTH) -= errDer.col(CurvePrimitive::DCURVATURE) * dcurvature;
        }
    }

    VectorXd params() const
    {
        if(_primitive.curve->getType() != CurvePrimitive::CLOTHOID)
            return _primitive.curve->params();
        VectorXd out = _primitive.curve->params();
        out(CurvePrimitive::DCURVATURE) = out(CurvePrimitive::CURVATURE) + out(CurvePrimitive::LENGTH) * out(CurvePrimitive::DCURVATURE);
        return out;
    }

    void setParams(const VectorXd &x)
    {
        if(_primitive.curve->getType() != CurvePrimitive::CLOTHOID)
        {
            _primitive.curve->setParams(x);
            return;
        }

        VectorXd xm = x;
        xm(CurvePrimitive::DCURVATURE) = (xm(CurvePrimitive::DCURVATURE) - xm(CurvePrimitive::CURVATURE)) / xm(CurvePrimitive::LENGTH);
        _primitive.curve->setParams(xm);
    }

private:
    FitPrimitive _primitive;
    ErrorComputerConstPtr _errorComputer;
};

class DefaultPrimitiveFitter : public Algorithm<PRIMITIVE_FITTING>
{
public:
    DefaultPrimitiveFitter(bool adjust) : _adjust(adjust) {}

    string name() const { return _adjust ? "Adjust" : "Default"; }

private:
    bool _adjust;

protected:

    void _run(const Fitter &fitter, AlgorithmOutput<PRIMITIVE_FITTING> &out)
    {
        const VectorC<bool> &corners = fitter.output<RESAMPLING>()->corners;
        PolylineConstPtr poly = fitter.output<RESAMPLING>()->output;
        ErrorComputerConstPtr errorComputer = fitter.output<ERROR_COMPUTER>()->errorComputer;

        const VectorC<Vector2d> &pts = poly->pts();

        const double errorThreshold = fitter.scaledParameter(Parameters::ERROR_THRESHOLD);
        std::string typeNames[3] = { "Lines", "Arcs", "Clothoids" };
        bool inflectionAccounting = fitter.params().get(Parameters::INFLECTION_COST) > 0.;

        for(int i = 0; i < pts.size(); ++i) //iterate over start points
        {
            FitterBasePtr fitters[3];
            fitters[0] = new LineFitter();
            fitters[1] = new ArcFitter();
            fitters[2] = new ClothoidFitter();

            for(int type = 0; type <= 2; ++type) //iterate over lines, arcs, clothoids
            {
                int fitSoFar = 0;

                bool needType = fitter.params().get(Parameters::ParameterType(Parameters::LINE_COST + type)) < Parameters::infinity;

                for(VectorC<Vector2d>::Circulator circ = pts.circulator(i); !circ.done(); ++circ)
                {
                    ++fitSoFar;

                    if(!needType && (type == 2 || fitSoFar >= 3 + type)) //if we don't need primitives of this type
                        break;

                    fitters[type]->addPoint(*circ);
                    if(fitSoFar >= 2 + type) //at least two points per line, etc.
                    {
                        CurvePrimitivePtr curve = fitters[type]->getPrimitive();
                        Vector3d color(0, 0, 0);
                        color[type] = 1;

                        FitPrimitive fit;
                        fit.curve = curve;
                        fit.startIdx = i;
                        fit.endIdx = circ.index();
                        fit.numPts = fitSoFar;
                        fit.startCurvSign = (curve->startCurvature() >= 0) ? 1 : -1;
                        fit.endCurvSign = (curve->endCurvature() >= 0) ? 1 : -1;

                        if(_adjust)
                            adjustPrimitive(fit, fitter);

                        fit.error = errorComputer->computeError(curve, i, fit.endIdx);

                        double length = poly->lengthFromTo(i, fit.endIdx);
                        if(fit.error / length > errorThreshold * errorThreshold)
                            break;

                        Debugging::get()->drawCurve(curve, color, typeNames[type]);
                        out.primitives.push_back(fit);

                        if(type == 0 && inflectionAccounting) //line with "opposite" curvature
                        {
                            fit.startCurvSign = -fit.startCurvSign;
                            fit.endCurvSign = -fit.endCurvSign;
                            out.primitives.push_back(fit);
                        }

                        //if different start and end curvatures
                        if(fit.startCurvSign != fit.endCurvSign && inflectionAccounting)
                        {
                            double start = poly->idxToParam(i);
                            double end = poly->idxToParam(fit.endIdx);
                            CurvePrimitivePtr startNoCurv = static_pointer_cast<ClothoidFitter>(fitters[2])->getCurveWithZeroCurvature(0);
                            CurvePrimitivePtr endNoCurv = static_pointer_cast<ClothoidFitter>(fitters[2])->getCurveWithZeroCurvature(end - start);

                            fit.curve = startNoCurv;
                            fit.startCurvSign = fit.endCurvSign = (startNoCurv->endCurvature() > 0. ? 1 : -1);

                            if(_adjust)
                                adjustPrimitive(fit, fitter);

                            fit.error = errorComputer->computeError(fit.curve, i, fit.endIdx);

                            if(fit.error / length < errorThreshold * errorThreshold)
                            {
                                out.primitives.push_back(fit);
                                Debugging::get()->drawCurve(fit.curve, color, typeNames[type]);
                            }

                            fit.curve = endNoCurv;
                            fit.startCurvSign = fit.endCurvSign = (endNoCurv->startCurvature() > 0. ? 1 : -1);

                            if(_adjust)
                                adjustPrimitive(fit, fitter);

                            fit.error = errorComputer->computeError(fit.curve, i, fit.endIdx);

                            if(fit.error / length < errorThreshold * errorThreshold)
                            {
                                out.primitives.push_back(fit);
                                Debugging::get()->drawCurve(fit.curve, color, typeNames[type]);
                            }
                        }
                    }
                    if(fitSoFar > 1 && corners[circ.index()])
                        break;
                }
            }
        }
    }

    void adjustPrimitive(const FitPrimitive &primitive, const Fitter &fitter)
    {
        ErrorComputerConstPtr errorComputer = fitter.output<ERROR_COMPUTER>()->errorComputer;
        bool inflectionAccounting = fitter.params().get(Parameters::INFLECTION_COST) > 0.;

        vector<LSBoxConstraint> constraints;

        //minimum length constraint
        constraints.push_back(LSBoxConstraint(CurvePrimitive::LENGTH, primitive.curve->length() * 0.5, 1));

        //curvature sign constraints
        if(inflectionAccounting)
        {
            if(primitive.curve->getType() >= CurvePrimitive::ARC)
                constraints.push_back(LSBoxConstraint(CurvePrimitive::CURVATURE, 0., primitive.startCurvSign));
            if(primitive.curve->getType() == CurvePrimitive::CLOTHOID)
                constraints.push_back(LSBoxConstraint(CurvePrimitive::DCURVATURE, 0., primitive.endCurvSign));
        }

        //solve
        OneCurveProblem problem(primitive, errorComputer);
        LSSolver solver(&problem, constraints);
        solver.setDefaultDamping(fitter.params().get(Parameters::CURVE_ADJUST_DAMPING));
        solver.setMaxIter(1);
        problem.setParams(solver.solve(problem.params()));
    }
};

void Algorithm<PRIMITIVE_FITTING>::_initialize()
{
    new DefaultPrimitiveFitter(false);
    new DefaultPrimitiveFitter(true);
}

END_NAMESPACE_Cornu


